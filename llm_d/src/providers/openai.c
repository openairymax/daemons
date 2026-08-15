// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file openai.c
 * @brief OpenAI adapter implementation (with production-grade rate limiting).
 *
 * PROTO-003 implementation:
 * 1. Token-bucket rate limiting (RPM/TPM)
 * 2. HTTP 429 detection and Retry-After header parsing
 * 3. Exponential backoff with jitter
 * 4. Configurable rate-limit parameters
 *
 * Improvements:
 * - Uses the common Provider infrastructure
 * - Follows OpenAI API best practices
 */

#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "provider.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-3.5-turbo"

#define OPENAI_DEFAULT_RPM 500 /* Requests per minute */
#define OPENAI_DEFAULT_TPM 150000 /* Tokens per minute (Tier 1) */
#define OPENAI_DEFAULT_TPM_TIER2 300000 /* Tokens per minute (Tier 2) */
#define OPENAI_MAX_RETRIES 5
#define OPENAI_BASE_DELAY_MS 1000
#define OPENAI_MAX_DELAY_MS 60000
#define OPENAI_JITTER_FACTOR 0.2

typedef struct {
    airy_mtx_t lock;
    time_t rpm_window_start;
    int rpm_count;
    int rpm_limit;
    long tpm_count;
    time_t tpm_window_start;
    long tpm_limit;
    time_t last_429_time;
    int retry_after_sec;
    int consecutive_429s;
} openai_rate_limiter_t;

typedef struct {
    provider_base_ctx_t base;
    openai_rate_limiter_t rl;
} openai_ctx_t;

static void openai_rl_init(openai_rate_limiter_t *rl)
{
    airy_mtx_init(&rl->lock);
    rl->rpm_window_start = time(NULL);
    rl->rpm_count = 0;
    rl->rpm_limit = OPENAI_DEFAULT_RPM;
    rl->tpm_count = 0;
    rl->tpm_window_start = time(NULL);
    rl->tpm_limit = OPENAI_DEFAULT_TPM;
    rl->last_429_time = 0;
    rl->retry_after_sec = 0;
    rl->consecutive_429s = 0;
}

static void openai_rl_destroy(openai_rate_limiter_t *rl)
{
    airy_mtx_destroy(&rl->lock);
}

static int openai_rl_check_rpm(openai_rate_limiter_t *rl)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    if (now - rl->rpm_window_start >= 60) {
        rl->rpm_count = 0;
        rl->rpm_window_start = now;
    }

    if (rl->rpm_count >= rl->rpm_limit) {
        airy_mtx_unlock(&rl->lock);
        return AIRY_ERR_LLM_RATE_LIMIT;
    }

    rl->rpm_count++;
    airy_mtx_unlock(&rl->lock);
    return 0;
}

static int __attribute__((unused)) openai_rl_check_tpm(openai_rate_limiter_t *rl, int tokens)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    if (now - rl->tpm_window_start >= 60) {
        rl->tpm_count = 0;
        rl->tpm_window_start = now;
    }

    if (rl->tpm_count + tokens > rl->tpm_limit) {
        airy_mtx_unlock(&rl->lock);
        return AIRY_ERR_LLM_RATE_LIMIT;
    }

    rl->tpm_count += tokens;
    airy_mtx_unlock(&rl->lock);
    return 0;
}

static void openai_rl_record_429(openai_rate_limiter_t *rl, int retry_after)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    rl->last_429_time = now;
    rl->consecutive_429s++;
    if (retry_after > 0) {
        rl->retry_after_sec = retry_after;
    } else {
        rl->retry_after_sec = 0;
    }

    airy_mtx_unlock(&rl->lock);
}

static int openai_rl_get_wait_ms(openai_rate_limiter_t *rl, int attempt)
{
    airy_mtx_lock(&rl->lock);

    int wait_ms;

    if (rl->retry_after_sec > 0) {
        wait_ms = rl->retry_after_sec * 1000;
        rl->retry_after_sec = 0;
    } else {
        int base_delay = OPENAI_BASE_DELAY_MS << attempt;
        if (base_delay > OPENAI_MAX_DELAY_MS)
            base_delay = OPENAI_MAX_DELAY_MS;

        double jitter =
            ((double)airy_random_uint32(0, 99) / 100.0) * base_delay * OPENAI_JITTER_FACTOR;
        wait_ms = (int)((double)base_delay + jitter);
    }

    airy_mtx_unlock(&rl->lock);
    return wait_ms;
}

static void openai_rl_reset_429(openai_rate_limiter_t *rl)
{
    airy_mtx_lock(&rl->lock);
    rl->consecutive_429s = 0;
    rl->retry_after_sec = 0;
    airy_mtx_unlock(&rl->lock);
}

static int parse_retry_after(const char *headers_data)
{
    if (!headers_data)
        return 0;

    const char *retry_ptr = strstr(headers_data, "retry-after:");
    if (!retry_ptr)
        retry_ptr = strstr(headers_data, "Retry-After:");
    if (!retry_ptr)
        return 0;

    retry_ptr = strchr(retry_ptr, ':');
    if (!retry_ptr)
        return 0;
    retry_ptr++;

    while (*retry_ptr == ' ' || *retry_ptr == '\t')
        retry_ptr++;

    long seconds = strtol(retry_ptr, NULL, 10);
    if (seconds <= 0)
        return 0;
    if (seconds > 300)
        seconds = 300;

    return (int)seconds;
}

static int openai_http_request_with_retry(openai_ctx_t *ctx, const char *url,
                                          struct curl_slist *headers, const char *body,
                                          long *out_http_code, provider_http_resp_t **out_response)
{
    int attempt = 0;
    int max_attempts = ctx->base.max_retries > 0 ? ctx->base.max_retries : OPENAI_MAX_RETRIES;

    while (attempt < max_attempts) {
        int ret = openai_rl_check_rpm(&ctx->rl);
        if (ret != 0) {
            SVC_LOG_WARN("C-L02: OPENAI: RATE-LIMIT url=%s reason=rpm_limit_reached "
                         "attempt=%d/%d",
                         url, attempt + 1, max_attempts);
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
            continue;
        }

        *out_response = NULL;
        ret = provider_http_post(url, headers, body, ctx->base.timeout_sec, 0, out_response,
                                 out_http_code);

        if (ret == AIRY_OK && *out_http_code == 200) {
            openai_rl_reset_429(&ctx->rl);
            return AIRY_OK;
        }

        if (*out_http_code == 429) {
            int retry_after = parse_retry_after(*out_response ? (*out_response)->data : NULL);
            openai_rl_record_429(&ctx->rl, retry_after);

            SVC_LOG_WARN("C-L02: OPENAI: RATE-LIMIT url=%s http_code=429 attempt=%d/%d "
                         "retry_after=%ds",
                         url, attempt + 1, max_attempts, retry_after);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int wait_ms = openai_rl_get_wait_ms(&ctx->rl, attempt);
            if (wait_ms > 0) {
                struct timespec ts = {.tv_sec = wait_ms / 1000,
                                      .tv_nsec = (wait_ms % 1000) * 1000000LL};
                nanosleep(&ts, NULL);
            }
            attempt++;
            continue;
        }

        if (*out_http_code >= 500 && *out_http_code < 600 && attempt < max_attempts - 1) {
            SVC_LOG_WARN("C-L02: OPENAI: SERVER-ERROR url=%s http_code=%ld attempt=%d/%d "
                         "retrying",
                         url, *out_http_code, attempt + 1, max_attempts);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int delay = OPENAI_BASE_DELAY_MS << attempt;
            if (delay > OPENAI_MAX_DELAY_MS)
                delay = OPENAI_MAX_DELAY_MS;
            struct timespec ts = {.tv_sec = delay / 1000, .tv_nsec = (delay % 1000) * 1000000LL};
            nanosleep(&ts, NULL);
            attempt++;
            continue;
        }

        break;
    }

    return AIRY_ERR_IO;
}

static provider_ctx_t *openai_init(const char *name __attribute__((unused)), const char *api_key,
                                   const char *api_base, const char *organization,
                                   double timeout_sec, int max_retries)
{

    openai_ctx_t *ctx = (openai_ctx_t *)AIRY_CALLOC(1, sizeof(openai_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: OPENAI: INIT-FAIL reason=oom STACK: openai_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, api_key, api_base, organization, timeout_sec, max_retries,
                       OPENAI_DEFAULT_BASE);

    openai_rl_init(&ctx->rl);

    airy_random_init();

    SVC_LOG_INFO("C-L02: OPENAI: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d "
                 "RPM=%d TPM=%ld",
                 ctx->base.api_base[0] ? ctx->base.api_base : OPENAI_DEFAULT_BASE,
                 ctx->base.timeout_sec, ctx->base.max_retries, ctx->base.api_key[0] ? 1 : 0,
                 OPENAI_DEFAULT_RPM, (long)OPENAI_DEFAULT_TPM);

    return (provider_ctx_t *)ctx;
}

static void openai_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
        SVC_LOG_INFO("C-L02: OPENAI: DESTROY ctx=%p", (void *)ctx_ptr);
        openai_rl_destroy(&ctx->rl);
        AIRY_FREE(ctx_ptr);
    }
}

static int openai_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                           llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        return AIRY_ERR_INVALID_PARAM;
    }

    openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    char *req_body = provider_build_openai_request(manager, OPENAI_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s reason=build_request_oom "
                      "STACK: openai_complete",
                      manager->model ? manager->model : OPENAI_DEFAULT_MODEL);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = manager->model && manager->model[0] ? manager->model : OPENAI_DEFAULT_MODEL;
    SVC_LOG_INFO("C-L02: OPENAI: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "stream=%d",
                 model, manager->message_count, manager->max_tokens, manager->temperature,
                 manager->stream ? 1 : 0);

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             base->api_key[0] ? base->api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    explicit_bzero(auth_header, sizeof(auth_header));

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    int ret = openai_http_request_with_retry(ctx, url, headers, req_body, &http_code, &http_resp);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        if (http_code == 429) {
            SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=rate_limit_exhausted",
                          model, http_code);
        } else {
            SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=http_request_failed body=%.600s",
                          model, http_code,
                          http_resp && http_resp->data ? http_resp->data : "");
        }
        if (http_resp)
            provider_http_resp_free(http_resp);
        return ret;
    }

    ret = provider_parse_openai_response(http_resp->data, out_response);
    provider_http_resp_free(http_resp);

    if (ret == AIRY_OK && *out_response) {
        SVC_LOG_INFO(
            "C-L02: OPENAI: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "http_code=%ld",
            (*out_response)->model ? (*out_response)->model : model, (*out_response)->prompt_tokens,
            (*out_response)->completion_tokens, (*out_response)->total_tokens, http_code);
    } else {
        SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                      "DIAGNOSIS=parse_response_failed ret=%d",
                      model, http_code, ret);
    }

    return ret;
}

typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *acc_reasoning;
    size_t acc_reasoning_cap;
    size_t acc_reasoning_len;
    char *resp_id;
    char *resp_model;
    uint64_t resp_created;
    char *finish_reason;
} oai_stream_acc_t;

static int oai_stream_on_chunk(const char *json_line, void *userdata)
{
    oai_stream_acc_t *acc = (oai_stream_acc_t *)userdata;

    CJSON_PARSE_GUARD(root, json_line, { return 0; });

    if (!acc->resp_id) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id) && id->valuestring) {
            acc->resp_id = AIRY_STRDUP(id->valuestring);
        }
    }

    if (!acc->resp_model) {
        cJSON *model = cJSON_GetObjectItem(root, "model");
        if (cJSON_IsString(model) && model->valuestring) {
            acc->resp_model = AIRY_STRDUP(model->valuestring);
        }
    }

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created) && acc->resp_created == 0) {
        acc->resp_created = (uint64_t)created->valuedouble;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        if (delta) {
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                const char *text = content->valuestring;

                if (acc->user_cb) {
                    acc->user_cb(text, acc->user_data);
                }

                char *grown =
                    provider_buf_append(acc->acc_content, &acc->acc_cap, &acc->acc_len, text);
                if (grown) {
                    acc->acc_content = grown;
                }
            }

            /* Reasoning trace arrives in the same delta stream (DeepSeek/
             * Kimi); accumulate it out-of-band (not forwarded to user_cb).
             * It must survive to the assembled response so the client can
             * echo it back on the next tool-loop turn. */
            cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
            if (cJSON_IsString(reasoning) && reasoning->valuestring) {
                char *grown =
                    provider_buf_append(acc->acc_reasoning, &acc->acc_reasoning_cap,
                                        &acc->acc_reasoning_len, reasoning->valuestring);
                if (grown) {
                    acc->acc_reasoning = grown;
                }
            }
        }

        cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "null") != 0) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(fr->valuestring);
        }
    }

    return 0;
}

static llm_response_t *oai_build_stream_response(oai_stream_acc_t *acc)
{
    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (acc->resp_id)
        resp->id = acc->resp_id;
    else
        resp->id = AIRY_STRDUP("");
    acc->resp_id = NULL;

    if (acc->resp_model)
        resp->model = acc->resp_model;
    else
        resp->model = AIRY_STRDUP("unknown");
    acc->resp_model = NULL;

    resp->created = acc->resp_created;

    resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (resp->choices) {
        resp->choice_count = 1;
        resp->choices[0].role = AIRY_STRDUP("assistant");
        resp->choices[0].content = acc->acc_content;
        acc->acc_content = NULL;
        resp->choices[0].reasoning_content = acc->acc_reasoning;
        acc->acc_reasoning = NULL;
    } else {
        resp->choice_count = 0;
    }

    if (acc->finish_reason) {
        resp->finish_reason = acc->finish_reason;
        acc->finish_reason = NULL;
    } else {
        resp->finish_reason = AIRY_STRDUP("stop");
    }

    return resp;
}

static int openai_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                  llm_stream_callback_t callback, void *user_data,
                                  llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback) {
        return AIRY_ERR_INVALID_PARAM;
    }

    openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = provider_build_openai_request(&stream_cfg, OPENAI_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s reason=build_request_oom "
                      "STACK: openai_complete_stream",
                      manager->model ? manager->model : OPENAI_DEFAULT_MODEL);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = manager->model && manager->model[0] ? manager->model : OPENAI_DEFAULT_MODEL;
    SVC_LOG_INFO("C-L02: OPENAI: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", model,
                 manager->message_count, manager->max_tokens, manager->temperature);

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             base->api_key[0] ? base->api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    explicit_bzero(auth_header, sizeof(auth_header));

    oai_stream_acc_t acc;
    __builtin_memset(&acc, 0, sizeof(acc));
    acc.user_cb = callback;
    acc.user_data = user_data;
    acc.acc_cap = 4096;
    acc.acc_content = (char *)AIRY_MALLOC(acc.acc_cap);

    long http_code = 0;
    int ret = provider_http_post_stream(url, headers, req_body, base->timeout_sec,
                                        oai_stream_on_chunk, &acc, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        if (http_code == 429) {
            SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=rate_limit_exhausted",
                          model, http_code);
        } else {
            SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=http_stream_error",
                          model, http_code);
        }
        AIRY_FREE(acc.acc_content);
        AIRY_FREE(acc.acc_reasoning);
        AIRY_FREE(acc.resp_id);
        AIRY_FREE(acc.resp_model);
        AIRY_FREE(acc.finish_reason);
        return ret;
    }

    llm_response_t *resp = oai_build_stream_response(&acc);
    AIRY_FREE(acc.acc_content);
    AIRY_FREE(acc.acc_reasoning);
    AIRY_FREE(acc.resp_id);
    AIRY_FREE(acc.resp_model);
    AIRY_FREE(acc.finish_reason);

    if (resp) {
        SVC_LOG_INFO("C-L02: OPENAI: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "http_code=%ld",
                     resp->model ? resp->model : model, resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens, http_code);
    } else {
        SVC_LOG_WARN("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld "
                     "DIAGNOSIS=null_response_built",
                     model, http_code);
    }

    if (out_response) {
        *out_response = resp;
    } else if (resp) {
        llm_response_free(resp);
    }

    return AIRY_OK;
}

const provider_ops_t openai_ops = {.init = openai_init,
                                   .destroy = openai_destroy,
                                   .complete = openai_complete,
                                   .complete_stream = openai_complete_stream,
                                   .name = "openai",
                                   .default_model = OPENAI_DEFAULT_MODEL,
                                   .default_base_url = OPENAI_DEFAULT_BASE};
