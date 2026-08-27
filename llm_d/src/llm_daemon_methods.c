// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file llm_daemon_methods.c
 * @brief llm_d daemon RPC-method domain (split from main.c, 2026-08-27):
 *        complete / complete_stream (SSE) / list_models / embeddings /
 *        count_tokens / health_check / get_stats handlers and the
 *        on_*_method dispatcher adapters.
 *
 * 2026-08-27 域拆分（main.c 1033 行 → 4 文件）：方法实现集中于此，
 * 入口引导在 main.c，请求解析在 llm_daemon_request.c，daemon 配置装配
 * 在 llm_daemon_config.c；共享符号经 llm_service_internal.h 声明。
 */

#include "airy_memory.h"
#include "error.h"
#include "llm_service_internal.h"
#include "response.h"
#include "token_counter.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "daemon_main.h"
#include "platform.h"

/* macOS/BSD 无 MSG_NOSIGNAL（Linux 专有 send flag）：非 Linux POSIX 平台
 * 定义为 0，避免编译失败。等价语义由 DAEMON_SETUP_SIGNALS 的
 * signal(SIGPIPE, SIG_IGN) 保证——peer 关闭时 send 返回 EPIPE 而非触发
 * SIGPIPE 杀进程（对齐 daemons/common platform_compat.c 的兜底做法）。 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define LLM_MAX_RETRIES 3
#define LLM_BASE_DELAY_MS 100

static char *handle_complete(cJSON *params, int id);
static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd);

static char *handle_list_models(cJSON *params __attribute__((unused)), int id)
{
    if (!g_service)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service not ready", id);

    char *json = llm_service_list_models(g_service);
    if (!json)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);

    cJSON *root = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!root)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid list result", id);

    /* Ownership semantics: jsonrpc_build_success attaches root to the
     * response object via cJSON_AddItemToObject and cJSON_Delete frees it
     * recursively; it must NOT be deleted here (otherwise double-free) */
    return jsonrpc_build_success(root, id);
}

void on_list_models_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_list_models(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Handle the embeddings method
 *
 * Forwards the OpenAI-format request ({"model":..,"input":..}) verbatim to
 * the owning provider's $api_base/embeddings and returns the upstream JSON.
 */
static char *handle_embeddings(cJSON *params, int id)
{
    if (!params || !g_service)
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);

    char *model = NULL;
    cJSON *m = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = AIRY_STRDUP(m->valuestring);

    char *body = cJSON_PrintUnformatted(params);
    if (!body) {
        AIRY_FREE(model);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    char *out = NULL;
    int rc = llm_service_embeddings(g_service, model, body, &out);
    AIRY_FREE(body);
    AIRY_FREE(model);

    if (rc != 0 || !out)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Embedding failed", id);

    cJSON *result = cJSON_Parse(out);
    AIRY_FREE(out);
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid embedding response", id);

    return jsonrpc_build_success(result, id);
}

void on_embeddings_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_embeddings(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static const char *llm_encoding_for_model(const char *model)
{
    if (!model || model[0] == '\0')
        return "cl100k_base";
    if (strstr(model, "claude"))
        return "claude";
    if (strstr(model, "gpt-3.5") || strstr(model, "text-davinci") || strstr(model, "code-davinci"))
        return "p50k_base";
    return "cl100k_base";
}

static char *handle_count_tokens(cJSON *params, int id)
{
    cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Missing text string", id);
    }

    const char *model = NULL;
    cJSON *model_json = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(model_json) && model_json->valuestring)
        model = model_json->valuestring;
    else if (g_service)
        model = llm_service_default_model(g_service);

    const char *encoding = llm_encoding_for_model(model);
    token_counter_t *counter = token_counter_create(encoding);
    if (!counter)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Token counter unavailable", id);

    size_t tokens = token_counter_count(counter, text->valuestring);
    token_counter_destroy(counter);
    if (tokens == (size_t)-1)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Token counting failed", id);

    cJSON *result = cJSON_CreateObject();
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    cJSON_AddStringToObject(result, "model", model ? model : "default");
    cJSON_AddStringToObject(result, "text", text->valuestring);
    cJSON_AddNumberToObject(result, "tokens", (double)tokens);
    cJSON_AddStringToObject(result, "encoding", encoding);
    return jsonrpc_build_success(result, id);
}

void on_count_tokens_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_count_tokens(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static char *handle_health_check(cJSON *params __attribute__((unused)), int id)
{
    cJSON *result = cJSON_CreateObject();
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    cJSON_AddStringToObject(result, "service", "llm_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    return jsonrpc_build_success(result, id);
}

void on_health_check_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_health_check(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static char *handle_get_stats(cJSON *params __attribute__((unused)), int id)
{
    if (!g_service)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service not ready", id);

    char *json = NULL;
    if (llm_service_stats(g_service, &json) != 0 || !json)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Get stats failed", id);

    cJSON *root = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!root)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid stats result", id);

    /* Ownership semantics: jsonrpc_build_success attaches root to the
     * response object via cJSON_AddItemToObject and cJSON_Delete frees it
     * recursively; it must NOT be deleted here */
    return jsonrpc_build_success(root, id);
}

void on_get_stats_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_get_stats(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Wrapper for the complete method (adapts the method_dispatcher
 *        interface)
 */
void on_complete_method(cJSON *params, int id, void *user_data __attribute__((unused)))
{
    char *response = handle_complete(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        size_t resp_len = strlen(response);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send start fd=%d resp_len=%zu", (int)client_fd,
                          resp_len);
        ssize_t sent = airy_sock_send(client_fd, response, resp_len);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send done fd=%d sent=%zd errno=%d",
                          (int)client_fd, sent, errno);
        AIRY_FREE(response);
    }
}

/**
 * @brief Wrapper for the complete_stream method
 */
void on_complete_stream_method(cJSON *params, int id, void *user_data __attribute__((unused)))
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    char *response = handle_complete_stream(params, id, client_fd);
    if (response) {
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Handle the complete method
 */
static char *handle_complete(cJSON *params, int id)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    uint64_t start_time = airy_time_ms();

    llm_response_t *resp = NULL;
    int ret = -1;

    for (int attempt = 0; attempt <= LLM_MAX_RETRIES; attempt++) {
        ret = llm_service_complete(g_service, &cfg, &resp);

        if (ret == 0)
            break;

        if (attempt < LLM_MAX_RETRIES) {
            unsigned delay_ms = LLM_BASE_DELAY_MS * (1U << (attempt > 15 ? 15 : attempt));
            SVC_LOG_WARN("LLM complete attempt %d/%d failed (err=%d), retrying in %ums",
                         attempt + 1, LLM_MAX_RETRIES + 1, ret, delay_ms);
            airy_sleep_ms(delay_ms);
        }
    }

    uint64_t end_time = airy_time_ms();

    if (ret != 0) {
        SVC_LOG_ERROR("LLM complete failed after %d attempts (total %llums)", LLM_MAX_RETRIES + 1,
                      (unsigned long long)(end_time - start_time));
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "LLM service unavailable after retries",
                                   id);
    }

    char *resp_json = response_to_json(resp);
    llm_response_free(resp);
    AIRY_FREE((void *)cfg.model);

    if (!resp_json) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Failed to serialize response", id);
    }

    CJSON_PARSE_GUARD(result, resp_json, {
        AIRY_FREE(resp_json);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid response format", id);
    });
    AIRY_FREE(resp_json);

    char *success = jsonrpc_build_success(result, id);
    /* P0.20.8 fix: jsonrpc_build_success transfers result ownership to root
     * via cJSON_AddItemToObject; cJSON_Delete(root) frees result recursively.
     * Must set NULL to prevent CJSON_AUTO_FREE cleanup from double-freeing
     * via cJSON_Delete(result). */
    result = NULL;

    request_context_destroy(ctx);
    return success;
}

/**
 * @brief Handle the complete_stream method
 */
typedef struct {
    airy_sock_t fd;
} llm_stream_ctx_t;

#ifndef _WIN32
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#endif

/**
 * @brief Blocking full send (streaming only)
 *
 * llm_d's streaming callback is driven by the curl write callback: the LLM
 * push rate can exceed the peer's (gateway SSE pull) consumption rate.
 * airy_sock_send uses MSG_DONTWAIT and returns EAGAIN when the buffer is full,
 * making the curl write callback fail -> STREAM-FAIL and the stream breaks.
 *
 * This switches to blocking mode: send loop + poll(POLLOUT) waiting for
 * writability, guaranteeing each incremental chunk is fully delivered before
 * returning. On peer close (EPIPE/ECONNRESET), silently give up without
 * triggering a stream failure.
 */
static void llm_stream_send_all(airy_sock_t fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int sent = send(fd, buf + off, (int)(len - off), 0);
#else
        ssize_t sent = send(fd, buf + off, len - off, MSG_NOSIGNAL);
#endif
        if (sent > 0) {
            off += (size_t)sent;
            continue;
        }
        if (sent < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEINTR)
                break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {

                struct pollfd pfd = {.fd = (int)fd, .events = POLLOUT};
                if (poll(&pfd, 1, 1000) <= 0)
                    break;
                continue;
            }
            if (errno == EINTR)
                continue;
            break;
#endif
        } else {
            break;
        }
    }
}

static void llm_stream_callback(const char *chunk, void *user_data)
{
    llm_stream_ctx_t *sctx = (llm_stream_ctx_t *)user_data;
    llm_stream_send_all(sctx->fd, chunk, strlen(chunk));
}

static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    cfg.stream = 1;

    llm_stream_ctx_t stream_ctx = {.fd = client_fd};

    llm_response_t *resp = NULL;
    int ret = llm_service_complete_stream(g_service, &cfg, llm_stream_callback, &stream_ctx, &resp);

    if (ret != 0) {
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service error", id);
    }

    if (resp) {
        /* 2.1.1.5 修复：流式结束发送 usage 控制帧（RS 'U' body RS），
         * 携带真实 token 消耗与计费金额——此前流式路径完全不回传 usage，
         * IPC 客户端的流式 token 统计与计费恒为 0。adapter 侧按帧协议
         * 解析；cost_usd 由 update_cost_tracking 回填（该帧同步带上，
         * 使 gateway SSE 透传的 usage 事件包含真实费用）。 */
        char usage_frame[288];
        int ufn = snprintf(usage_frame, sizeof(usage_frame),
                           "\x1eU{\"prompt_tokens\":%u,\"completion_tokens\":%u,"
                           "\"total_tokens\":%u,\"reasoning_tokens\":%u,\"cost_usd\":%.8f}\x1e",
                           resp->prompt_tokens, resp->completion_tokens, resp->total_tokens,
                           resp->reasoning_tokens, resp->cost_usd);
        if (ufn > 0)
            llm_stream_send_all(client_fd, usage_frame, (size_t)ufn);
        llm_response_free(resp);
    }

    AIRY_FREE((void *)cfg.model);
    request_context_destroy(ctx);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}
