// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_openai_compat.h"

#include "airy_memory.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

#include "logging.h"

struct gw_openai_compat {
    gw_openai_compat_config_t config;
    gw_openai_llm_call_fn llm_call_fn;
    void *llm_call_data;
    gw_openai_embed_fn embed_fn;
    void *embed_data;
    bool initialized;
    bool healthy;
    uint64_t request_count;
    uint64_t error_count;
    uint64_t tokens_used;
    time_t window_start;
    uint32_t window_requests;
    airy_mtx_t lock; /**< MHD 线程池并发下保护限流窗口与统计计数 */
};

static int handle_openai_request(const char *method, const char *path, const char *body_json,
                                 char **response_json, void *user_data);

gw_openai_compat_t *gw_openai_compat_create(const gw_openai_compat_config_t *config)
{
    gw_openai_compat_t *compat = (gw_openai_compat_t *)AIRY_CALLOC(1, sizeof(gw_openai_compat_t));
    if (!compat) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (config) {
        compat->config = *config;
    } else {
        gw_openai_compat_config_t defaults = GW_OPENAI_COMPAT_CONFIG_DEFAULTS;
        compat->config = defaults;
    }
    return compat;
}

void gw_openai_compat_destroy(gw_openai_compat_t *compat)
{
    if (!compat)
        return;
    if (compat->initialized) {
        gw_openai_compat_shutdown(compat);
    }
    airy_mtx_destroy(&compat->lock);
    AIRY_FREE(compat);
}

int gw_openai_compat_init(gw_openai_compat_t *compat)
{
    if (!compat)
        return AIRY_ERR_INVALID_PARAM;
    if (compat->initialized)
        return 0;
    airy_mtx_init(&compat->lock);
    compat->initialized = true;
    compat->healthy = true;
    compat->request_count = 0;
    compat->error_count = 0;
    compat->tokens_used = 0;
    compat->window_start = time(NULL);
    compat->window_requests = 0;
    return 0;
}

int gw_openai_compat_shutdown(gw_openai_compat_t *compat)
{
    if (!compat || !compat->initialized)
        return AIRY_ERR_INVALID_PARAM;
    compat->initialized = false;
    compat->healthy = false;
    return 0;
}

int gw_openai_compat_set_llm_call(gw_openai_compat_t *compat, gw_openai_llm_call_fn fn,
                                  void *user_data)
{
    if (!compat || !fn)
        return AIRY_ERR_INVALID_PARAM;
    compat->llm_call_fn = fn;
    compat->llm_call_data = user_data;
    return 0;
}

int gw_openai_compat_set_embed_fn(gw_openai_compat_t *compat, gw_openai_embed_fn fn,
                                  void *user_data)
{
    if (!compat || !fn)
        return AIRY_ERR_INVALID_PARAM;
    compat->embed_fn = fn;
    compat->embed_data = user_data;
    return 0;
}

static bool check_rate_limit(gw_openai_compat_t *compat)
{
    bool allowed = false;
    airy_mtx_lock(&compat->lock);
    time_t now = time(NULL);
    if (now - compat->window_start >= 60) {
        compat->window_start = now;
        compat->window_requests = 0;
    }
    if (compat->window_requests < compat->config.rate_limit_rpm) {
        compat->window_requests++;
        allowed = true;
    }
    airy_mtx_unlock(&compat->lock);
    return allowed;
}

static char *extract_json_field_string(const char *json, const char *field)
{
    if (!json || !field) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    size_t flen = strlen(field) + 4;
    char *key = (char *)AIRY_MALLOC(flen);
    if (!key) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    snprintf(key, flen, "\"%s\"", field);
    const char *p = strstr(json, key);
    AIRY_FREE(key);
    if (!p) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p += strlen(field) + 3;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '"') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    size_t len = (size_t)(end - p);
    char *val = (char *)AIRY_MALLOC(len + 1);
    if (!val) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    AIRY_MEMCPY(val, p, len);
    val[len] = '\0';
    return val;
}

static double extract_json_field_number(const char *json, const char *field, double default_val)
{
    if (!json || !field)
        return default_val;
    size_t flen = strlen(field) + 4;
    char *key = (char *)AIRY_MALLOC(flen);
    if (!key)
        return default_val;
    snprintf(key, flen, "\"%s\"", field);
    const char *p = strstr(json, key);
    AIRY_FREE(key);
    if (!p)
        return default_val;
    p += strlen(field) + 3;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    char *endptr = NULL;
    double val = strtod(p, &endptr);
    if (endptr == p)
        return default_val;
    return val;
}

static int extract_json_field_int(const char *json, const char *field, int default_val)
{
    if (!json || !field)
        return default_val;
    size_t flen = strlen(field) + 4;
    char *key = (char *)AIRY_MALLOC(flen);
    if (!key)
        return default_val;
    snprintf(key, flen, "\"%s\"", field);
    const char *p = strstr(json, key);
    AIRY_FREE(key);
    key = NULL;
    if (!p)
        return default_val;
    p += strlen(field) + 3;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    char *endptr = NULL;
    long val = strtol(p, &endptr, 10);
    if (endptr == p)
        return default_val;
    return (int)val;
}

static char *extract_messages_array(const char *json)
{
    if (!json) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    const char *key = "\"messages\"";
    const char *p = strstr(json, key);
    if (!p) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '[') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '[')
            depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0) {
                p++;
                size_t len = (size_t)(p - start);
                char *arr = (char *)AIRY_MALLOC(len + 1);
                if (!arr) {
                    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
                }
                AIRY_MEMCPY(arr, start, len);
                arr[len] = '\0';
                return arr;
            }
        }
        p++;
    }
    return NULL;
}

static char *extract_functions_array(const char *json)
{
    if (!json) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    const char *key = "\"functions\"";
    const char *p = strstr(json, key);
    if (!p) {
        key = "\"tools\"";
        p = strstr(json, key);
    }
    if (!p) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '[') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '[')
            depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0) {
                p++;
                size_t len = (size_t)(p - start);
                char *arr = (char *)AIRY_MALLOC(len + 1);
                if (!arr) {
                    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
                }
                AIRY_MEMCPY(arr, start, len);
                arr[len] = '\0';
                return arr;
            }
        }
        p++;
    }
    return NULL;
}

static int handle_chat_completions(gw_openai_compat_t *compat, const char *body_json,
                                   char **response_json)
{
    if (!compat->llm_call_fn) {
        AIRY_LOG_ERROR("no LLM backend configured for chat completions");
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"No LLM backend configured\","
                                     "\"type\":\"server_error\",\"code\":503}}");
        sync_atomic_add(&compat->error_count, 1);
        return AIRY_ERR_NULL_POINTER;
    }

    if (!check_rate_limit(compat)) {
        AIRY_LOG_WARN("rate limit exceeded: window_requests=%u, limit=%u", compat->window_requests,
                 compat->config.rate_limit_rpm);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Rate limit exceeded\","
                                     "\"type\":\"rate_limit_error\",\"code\":429}}");
        sync_atomic_add(&compat->error_count, 1);
        return AIRY_ERR_OVERFLOW;
    }

    char *model = extract_json_field_string(body_json, "model");
    char *messages = extract_messages_array(body_json);
    char *functions = extract_functions_array(body_json);

    double temperature =
        extract_json_field_number(body_json, "temperature", compat->config.temperature_default);
    int max_tokens =
        extract_json_field_int(body_json, "max_tokens", (int)compat->config.max_tokens_default);

    char *llm_response = NULL;
    int rc = compat->llm_call_fn(model ? model : compat->config.default_model,
                                 messages ? messages : "[]", functions ? functions : NULL,
                                 temperature, max_tokens, &llm_response, compat->llm_call_data);

    if (rc != 0 || !llm_response) {
        /* Note: model/messages/functions are still valid here; print them
         * in the error branch before freeing, to avoid use-after-free (the
         * previous code freed then referenced, causing an ASan
         * heap-use-after-free crash). */
        AIRY_LOG_ERROR("LLM call failed: model=%s, rc=%d", model ? model : compat->config.default_model,
                  rc);
        AIRY_FREE(llm_response);
        AIRY_FREE(model);
        AIRY_FREE(messages);
        AIRY_FREE(functions);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"LLM call failed\","
                                     "\"type\":\"server_error\",\"code\":500}}");
        sync_atomic_add(&compat->error_count, 1);
        return AIRY_ERR_IO;
    }

    AIRY_FREE(model);
    AIRY_FREE(messages);
    AIRY_FREE(functions);
    *response_json = llm_response;
    sync_atomic_add(&compat->tokens_used, (uint64_t)(strlen(llm_response) / 4));
    return 0;
}

static int handle_embeddings(gw_openai_compat_t *compat, const char *body_json,
                             char **response_json)
{
    if (!compat->embed_fn) {
        AIRY_LOG_ERROR("no embedding backend configured for embeddings endpoint");
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"No embedding backend configured\","
                                     "\"type\":\"server_error\",\"code\":503}}");
        sync_atomic_add(&compat->error_count, 1);
        return AIRY_ERR_NULL_POINTER;
    }

    char *model = extract_json_field_string(body_json, "model");
    char *input_start = strstr(body_json, "\"input\"");
    char *input_json = NULL;
    if (input_start) {
        input_start += 7;
        while (*input_start && (*input_start == ' ' || *input_start == ':' || *input_start == '\t'))
            input_start++;
        if (*input_start == '"' || *input_start == '[') {
            const char *start = input_start;
            if (*input_start == '[') {
                int depth = 0;
                while (*input_start) {
                    if (*input_start == '[')
                        depth++;
                    else if (*input_start == ']') {
                        depth--;
                        if (depth == 0) {
                            input_start++;
                            break;
                        }
                    }
                    input_start++;
                }
            } else {
                input_start++;
                char *end = strchr(input_start, '"');
                if (end)
                    input_start = end + 1;
            }
            size_t len = (size_t)(input_start - start);
            input_json = (char *)AIRY_MALLOC(len + 1);
            if (input_json) {
                AIRY_MEMCPY(input_json, start, len);
                input_json[len] = '\0';
            }
        }
    }

    char *embed_response = NULL;
    int rc = compat->embed_fn(model ? model : "text-embedding-ada-002",
                              input_json ? input_json : "[]", &embed_response, compat->embed_data);

    if (rc != 0 || !embed_response) {
        AIRY_LOG_ERROR("embedding call failed: model=%s, rc=%d",
                  model ? model : "text-embedding-ada-002", rc);
        AIRY_FREE(embed_response);
        AIRY_FREE(model);
        model = NULL;
        AIRY_FREE(input_json);
        input_json = NULL;
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Embedding failed\","
                                     "\"type\":\"server_error\",\"code\":500}}");
        sync_atomic_add(&compat->error_count, 1);
        return AIRY_ERR_IO;
    }

    AIRY_FREE(model);
    model = NULL;
    AIRY_FREE(input_json);
    input_json = NULL;

    *response_json = embed_response;
    return 0;
}

static int handle_models_list(gw_openai_compat_t *compat, char **response_json)
{
    const char *resp = "{\"object\":\"list\",\"data\":["
                       "{\"id\":\"%s\",\"object\":\"model\",\"owned_by\":\"agentrt\"}]}";
    size_t len = snprintf(NULL, 0, resp, compat->config.default_model);
    char *buf = (char *)AIRY_MALLOC(len + 1);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(buf, len + 1, resp, compat->config.default_model);
    *response_json = buf;
    return 0;
}

int gw_openai_compat_handle_request(gw_openai_compat_t *compat, const char *method,
                                    const char *path, const char *body_json, char **response_json)
{
    if (!compat || !response_json)
        return AIRY_ERR_INVALID_PARAM;
    sync_atomic_add(&compat->request_count, 1);

    if (!method)
        method = "POST";

    if (path && strcmp(path, "/v1/models") == 0) {
        return handle_models_list(compat, response_json);
    }

    if (path && (strcmp(path, "/v1/chat/completions") == 0 ||
                 strcmp(path, "/openai/v1/chat/completions") == 0)) {
        if (!body_json) {
            *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Empty body\","
                                         "\"type\":\"invalid_request_error\",\"code\":400}}");
            sync_atomic_add(&compat->error_count, 1);
            return AIRY_ERR_INVALID_PARAM;
        }
        return handle_chat_completions(compat, body_json, response_json);
    }

    if (path &&
        (strcmp(path, "/v1/embeddings") == 0 || strcmp(path, "/openai/v1/embeddings") == 0)) {
        if (!body_json) {
            *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Empty body\","
                                         "\"type\":\"invalid_request_error\",\"code\":400}}");
            sync_atomic_add(&compat->error_count, 1);
            return AIRY_ERR_INVALID_PARAM;
        }
        return handle_embeddings(compat, body_json, response_json);
    }

    if (body_json && strstr(body_json, "\"messages\"") && strstr(body_json, "\"model\"")) {
        return handle_chat_completions(compat, body_json, response_json);
    }

    *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Unknown OpenAI endpoint\","
                                 "\"type\":\"invalid_request_error\",\"code\":404}}");
    sync_atomic_add(&compat->error_count, 1);
    return AIRY_ERR_NOT_FOUND;
}

static int handle_openai_request(const char *method, const char *path, const char *body_json,
                                 char **response_json, void *user_data)
{
    gw_openai_compat_t *compat = (gw_openai_compat_t *)user_data;
    if (!compat)
        return AIRY_ERR_NULL_POINTER;
    return gw_openai_compat_handle_request(compat, method, path, body_json, response_json);
}

gw_proto_request_handler_t gw_openai_compat_get_handler(gw_openai_compat_t *compat)
{
    if (!compat) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    return handle_openai_request;
}

void *gw_openai_compat_get_handler_data(gw_openai_compat_t *compat)
{
    return (void *)compat;
}

bool gw_openai_compat_is_healthy(gw_openai_compat_t *compat)
{
    if (!compat)
        return false;
    return compat->healthy;
}
