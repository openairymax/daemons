// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file methods.c
 * @brief llm.* RPC 方法域：complete / complete_stream（SSE）/
 *        list_models / embeddings / count_tokens / health_check /
 *        get_stats 的 handler 与 m_* 薄壳。
 *
 * 0.1.19 gen5：方法实现集中于此，生命周期/端点在 src/svc.c，请求解析在
 * src/rpc/dispatch.c；m_* 签名与 .manifest rpc.methods 一一对应
 * （svc_llm_d.h 由生成器产出，L3 SSoT），壳体经 DAEMON_RPC_SHELL 生成。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"
#include "providers/core/prov_diag.h"
#include "response.h"
#include "token.h"
#include "token_standard.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "daemon_main.h"
#include "platform.h"

/* macOS/BSD 无 MSG_NOSIGNAL（Linux 专有 flag）：定义为 0；对端关闭时的
 * SIGPIPE 由 DAEMON_SETUP_SIGNALS 的 signal(SIGPIPE, SIG_IGN) 兜底。 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define LLM_MAX_RETRIES 3
#define LLM_BASE_DELAY_MS 100

/* R-5：complete 总耗时须严格小于网关转发超时背压（GW_LLM_DEFAULT_TIMEOUT_MS
 * = 90s），否则网关先放弃，provider 侧精确诊断（鉴权/限流/连接）回传不到
 * 用户。最坏总耗时 = LLM_MAX_RETRIES × 本上界 + 单次超时 = 3 × 5s + 70s
 * = 85s < 90s；单次尝试已接近完整超时（对端不可达/挂起）时不再重试。 */
#define LLM_RETRY_FAST_FAIL_MS 5000

/* 流式失败按 AIRY_ERR_* 透传具体原因而非统一 -32603 "Service error"，
 * 未知错误附错误码，便于用户与 TUI 定位根因。 */
static const char *llm_error_message(int ret)
{
    switch (ret) {
    case AIRY_ERR_INVALID_PARAM:
        return "Missing model: configure AIRY_AGENT_MODEL or config/model.yaml";
    case AIRY_ERR_LLM_INVALID_MODEL:
        return "Model not found in provider registry (check config/model.yaml and provider API key)";
    case AIRY_ERR_LLM_NO_PROVIDER:
        return "No LLM provider configured (set provider API key in config/secrets.env)";
    case AIRY_ERR_LLM_AUTH_FAIL:
        return "Provider authentication failed (check API key in config/secrets.env)";
    case AIRY_ERR_LLM_RATE_LIMIT:
        return "Provider rate limited (HTTP 429), retry later";
    case AIRY_ERR_LLM_BAD_REQUEST:
        return "Provider rejected the request body (HTTP 400/422): malformed JSON or "
               "invalid UTF-8 in messages; check the message content";
    case AIRY_ERR_LLM_CONTEXT_LEN:
        return "Context length exceeded for model";
    case AIRY_ERR_NOT_SUPPORTED:
        return "Selected provider does not support streaming";
    case AIRY_ERR_IO:
        return "Network request to provider failed (check connectivity and provider base_url)";
    case AIRY_ERR_TIMEOUT:
        return "Provider request timed out";
    default:
        return NULL;
    }
}

/* 未知错误码拼接进调用方栈缓冲，仅在同一次响应构建内使用。 */
static const char *llm_error_message_fmt(int ret, char *buf, size_t buf_len)
{
    const char *msg = llm_error_message(ret);
    if (msg)
        return msg;
    snprintf(buf, buf_len, "LLM request failed (error=%d)", ret);
    return buf;
}

/* B1-d：失败面文案 = AIRY_ERR_* 映射 + 本线程上游诊断片段（若有）。
 * 诊断由 providers/core 失败点在释放 resp 前写入，handler 发起服务
 * 调用前已 clear，读到的必是本次请求最后一次上游失败。 */
static const char *llm_fail_msg(int ret, char *buf, size_t buf_len)
{
    const char *diag = prov_diag_text();
    if (!diag[0])
        return llm_error_message_fmt(ret, buf, buf_len);
    char base[160];
    const char *msg = llm_error_message_fmt(ret, base, sizeof(base));
    snprintf(buf, buf_len, "%s | %s", msg, diag);
    return buf;
}

/* R-4：只有瞬态错误才值得外层重试。确定性失败（鉴权/模型/参数）重试只会
 * 把一次请求放大成 4 × 5 次打点风暴，拖慢报错并可能触发 provider 风控。 */
static int llm_error_is_retryable(int ret)
{
    switch (ret) {
    case AIRY_ERR_LLM_AUTH_FAIL:
    case AIRY_ERR_LLM_INVALID_MODEL:
    case AIRY_ERR_LLM_NO_PROVIDER:
    case AIRY_ERR_LLM_CONTEXT_LEN:
    case AIRY_ERR_LLM_TOKEN_LIMIT:
    case AIRY_ERR_LLM_BAD_REQUEST:
    case AIRY_ERR_INVALID_PARAM:
    case AIRY_ERR_NOT_SUPPORTED:
        return 0;
    default:
        return 1;
    }
}

/* 服务层 JSON 文本直发：root 所有权移交 jsonrpc_build_success，此处不得
 * 再 delete。 */
static void send_service_json(char *json, int id, airy_sock_t fd, const char *err,
                              const char *bad)
{
    if (!json) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, err, id);
        return;
    }

    cJSON *root = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!root) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, bad, id);
        return;
    }
    JSONRPC_SEND_SUCCESS(fd, root, id);
}

static void handle_list_models(cJSON *params, int id, airy_sock_t fd)
{
    (void)params;
    if (!g_service) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Service not ready", id);
        return;
    }

    send_service_json(llm_service_list_models(g_service), id, fd, "Out of memory",
                      "Invalid list result");
}

/* 把 OpenAI 格式请求原样转发到归属 provider 的 $api_base/embeddings。 */
static void handle_embeddings(cJSON *params, int id, airy_sock_t fd)
{
    JSONRPC_REQUIRE(params && g_service, fd, "Invalid params", id);

    char *model = NULL;
    cJSON *m = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = AIRY_STRDUP(m->valuestring);

    char *body = cJSON_PrintUnformatted(params);
    if (!body) {
        AIRY_FREE(model);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    char *out = NULL;
    prov_diag_clear();
    int rc = llm_service_embeddings(g_service, model, body, &out);
    AIRY_FREE(body);
    AIRY_FREE(model);

    if (rc != 0 || !out) {
        AIRY_FREE(out);
        char ebuf[640];
        const char *msg = (rc != 0) ? llm_fail_msg(rc, ebuf, sizeof(ebuf)) : "Embedding failed";
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, msg, id);
        return;
    }
    send_service_json(out, id, fd, "Out of memory", "Invalid embedding response");
}

static void handle_count_tokens(cJSON *params, int id, airy_sock_t fd)
{
    cJSON *text = cJSON_GetObjectItem(params, "text");
    JSONRPC_REQUIRE(cJSON_IsString(text) && text->valuestring, fd, "Missing text string", id);

    cJSON *model_json = cJSON_GetObjectItem(params, "model");
    const char *model = (cJSON_IsString(model_json) && model_json->valuestring)
                            ? model_json->valuestring
                            : (g_service ? llm_service_default_model(g_service) : NULL);
    /* 分词族由模型策略声明（products 层掌握厂商知识），未声明落默认族。 */
    cJSON *family_json = cJSON_GetObjectItem(params, "family");
    const char *family = (cJSON_IsString(family_json) && family_json->valuestring &&
                          family_json->valuestring[0])
                             ? family_json->valuestring
                             : AIRY_TOKEN_MODEL_NAME_DEFAULT;
    airy_token_counter_t *counter = airy_token_counter_create(family);
    if (!counter) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Unknown token family", id);
        return;
    }
    size_t tokens = airy_token_counter_count(counter, text->valuestring);
    airy_token_counter_destroy(counter);
    if (tokens == (size_t)-1) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Token counting failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "model", model ? model : "default");
    cJSON_AddStringToObject(result, "family", family);
    cJSON_AddStringToObject(result, "text", text->valuestring);
    cJSON_AddNumberToObject(result, "tokens", (double)tokens);
    cJSON_AddStringToObject(result, "algorithm", airy_token_get_algorithm_info());
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_health_check(cJSON *params, int id, airy_sock_t fd)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "service", "llm_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    /* D5 端点健康视图：回应"能力存在 ≠ 能力可用"。无匹配端点时省略
     * endpoints 字段；endpoint_probe 单独声明探测能力是否在线，供消费方
     * 区分"无端点"与"探测降级禁用"。 */
    cJSON_AddBoolToObject(result, "endpoint_probe", g_ep != NULL);
    char *ej = ep_health_json(g_ep);
    if (ej) {
        cJSON *endpoints = cJSON_Parse(ej);
        AIRY_FREE(ej);
        if (endpoints)
            cJSON_AddItemToObject(result, "endpoints", endpoints);
    }

    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_get_stats(cJSON *params, int id, airy_sock_t fd)
{
    (void)params;
    if (!g_service) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Service not ready", id);
        return;
    }

    char *json = NULL;
    if (llm_service_stats(g_service, &json) != 0) {
        AIRY_FREE(json);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Get stats failed", id);
        return;
    }

    send_service_json(json, id, fd, "Get stats failed", "Invalid stats result");
}

/* complete：外层重试；参数解析失败在 -32602 中携带具体环节（P24，0.1.12），
 * fail_reason 需在 request_context_destroy 之前拷贝到栈缓冲。 */
static void handle_complete(cJSON *params, int id, airy_sock_t fd)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        char ebuf[200];
        snprintf(ebuf, sizeof(ebuf), "Invalid params: %s",
                 (ctx && ctx->fail_reason[0]) ? ctx->fail_reason : "request params rejected");
        SVC_LOG_ERROR("complete: %s", ebuf);
        request_context_destroy(ctx);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, ebuf, id);
        return;
    }

    uint64_t start_time = airy_time_ms();

    prov_diag_clear();

    llm_response_t *resp = NULL;
    int ret = -1;
    int attempts_used = 0;

    for (int attempt = 0; attempt <= LLM_MAX_RETRIES; attempt++) {
        attempts_used = attempt + 1;
        uint64_t attempt_start = airy_time_ms();
        ret = llm_service_complete(g_service, &cfg, &resp);

        if (ret == 0)
            break;

        if (!llm_error_is_retryable(ret)) {
            SVC_LOG_ERROR("LLM complete aborted: non-retryable error (err=%d)", ret);
            break;
        }

        if (attempt < LLM_MAX_RETRIES) {
            unsigned delay_ms = LLM_BASE_DELAY_MS * (1U << (attempt > 15 ? 15 : attempt));
            /* R-5：慢失败不再重试（见 LLM_RETRY_FAST_FAIL_MS 上界推导）。 */
            uint64_t attempt_ms = airy_time_ms() - attempt_start;
            if (attempt_ms >= LLM_RETRY_FAST_FAIL_MS) {
                SVC_LOG_WARN("LLM complete not retrying (err=%d): attempt took %llums, "
                             "reporting diagnosis before gateway timeout",
                             ret, (unsigned long long)attempt_ms);
                break;
            }
            SVC_LOG_WARN("LLM complete attempt %d/%d failed (err=%d), retrying in %ums",
                         attempt + 1, LLM_MAX_RETRIES + 1, ret, delay_ms);
            airy_sleep_ms(delay_ms);
        }
    }

    uint64_t end_time = airy_time_ms();

    if (ret != 0) {
        SVC_LOG_ERROR("LLM complete failed after %d attempts (total %llums)", attempts_used,
                      (unsigned long long)(end_time - start_time));
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        char ebuf[640];
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, llm_fail_msg(ret, ebuf, sizeof(ebuf)), id);
        return;
    }

    char *resp_json = response_to_json(resp);
    llm_response_free(resp);
    AIRY_FREE((void *)cfg.model);
    request_context_destroy(ctx);
    send_service_json(resp_json, id, fd, "Failed to serialize response",
                      "Invalid response format");
}

/* 流式会话上下文：D5 首 token 观测与冷加载上报的端点归属。 */
typedef struct {
    airy_sock_t fd;
    /* D5 首 token 观测：first_ms 于首个 chunk 一次性落定（0 = 未到达）。 */
    uint64_t start_ms;
    uint64_t first_ms;
    char provider[64];
    char model[128];
} llm_stream_ctx_t;

#ifndef _WIN32
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#endif

/* 阻塞式全量发送（仅流式）：curl write 回调的推速可超过网关 SSE 拉速，
 * airy_sock_send 的 MSG_DONTWAIT 在缓冲区满时返回 EAGAIN，会让回调失败并
 * 中断流。此处改为 send 循环 + poll(POLLOUT) 等待可写；对端关闭
 * （EPIPE/ECONNRESET）时静默放弃，不制造流失败。 */
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

    /* D5：首个 chunk 到达即首 token 延迟；冷加载只观测不阻断，阈值判定
     * 归机制件（避免常量双写）。 */
    if (sctx->first_ms == 0 && sctx->start_ms != 0) {
        sctx->first_ms = airy_time_ms() - sctx->start_ms;
        ep_health_cold(g_ep, sctx->provider, sctx->model, (double)sctx->first_ms);
    }

    llm_stream_send_all(sctx->fd, chunk, strlen(chunk));
}

/* P24（0.1.12）：流式路径的错误必须以 RS 'E' 帧下发——裸 JSON-RPC 信封会被
 * adapter 流解复用器当正文转发给客户端（社区 ubuntu v0.1.11 实证）；旧
 * adapter 不识别 'E' 标签则整帧丢弃（可接受的降级）。帧体由 jsonrpc_build_error
 * 构造，避免把 message 以 %s 裸拼进 JSON 时转义责任落到各调用点。
 *
 * '\x1e' 必须写成字符字面量：\x 转义会吞掉紧随的十六进制字符（'E' 也是
 * hex digit），"\x1eE{..." 被解析成 0x1EE（clang #132 macOS 实证）。 */
static void llm_stream_send_error_frame(airy_sock_t fd, int id, int code, const char *message)
{
    char *body = jsonrpc_build_error(code, message, id);
    if (!body)
        return;

    const size_t blen = strlen(body);
    char buf[512];
    if (blen + 3 <= sizeof(buf)) {
        buf[0] = '\x1e';
        buf[1] = 'E';
        AIRY_MEMCPY(buf + 2, body, blen);
        buf[blen + 2] = '\x1e';
        llm_stream_send_all(fd, buf, blen + 3);
    }
    AIRY_FREE(body);
}

static void handle_complete_stream(cJSON *params, int id, airy_sock_t fd)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        /* 流协议只认 RS 帧：错误走 'E' 帧，不回文本响应。 */
        const char *reason = (ctx && ctx->fail_reason[0]) ? ctx->fail_reason
                                                          : "request params rejected";
        char rbuf[200];
        snprintf(rbuf, sizeof(rbuf), "Invalid params: %s", reason);
        SVC_LOG_ERROR("complete_stream: %s", rbuf);
        llm_stream_send_error_frame(fd, id, JSONRPC_INVALID_PARAMS, rbuf);
        request_context_destroy(ctx);
        return;
    }

    cfg.stream = 1;

    llm_stream_ctx_t stream_ctx = {.fd = fd, .start_ms = airy_time_ms()};

    /* D5：解析本请求归属端点（模型 → provider）供首 token 冷加载上报；
     * 未命中留空，机制件仅记日志不新增记录（口径与 filter 一致）。 */
    const provider_t *prov =
        cfg.model ? provider_registry_find(llm_svc_reg(g_service), cfg.model) : NULL;
    if (prov && prov->name)
        AIRY_STRNCPY_TERM(stream_ctx.provider, prov->name, sizeof(stream_ctx.provider));
    if (cfg.model)
        AIRY_STRNCPY_TERM(stream_ctx.model, cfg.model, sizeof(stream_ctx.model));

    prov_diag_clear();

    llm_response_t *resp = NULL;
    int ret = llm_service_complete_stream(g_service, &cfg, llm_stream_callback, &stream_ctx, &resp);

    if (ret != 0) {
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        /* 与 parse_params 失败路径同因同治：错误只能走 RS 'E' 帧。 */
        char ebuf[640];
        const char *msg = llm_fail_msg(ret, ebuf, sizeof(ebuf));
        SVC_LOG_ERROR("complete_stream: %s", msg);
        llm_stream_send_error_frame(fd, id, JSONRPC_INTERNAL_ERROR, msg);
        return;
    }

    if (resp) {
        /* 流式结束发 usage 控制帧（RS 'U'）：携带真实 token 与回填后的
         * cost_usd，否则客户端统计与计费恒为 0。 */
        char usage_frame[288];
        int ufn = snprintf(usage_frame, sizeof(usage_frame),
                           "\x1eU{\"prompt_tokens\":%u,\"completion_tokens\":%u,"
                           "\"total_tokens\":%u,\"reasoning_tokens\":%u,"
                           "\"cached_tokens\":%u,\"cost_usd\":%.8f}\x1e",
                           resp->prompt_tokens, resp->completion_tokens, resp->total_tokens,
                           resp->reasoning_tokens, resp->cached_tokens, resp->cost_usd);
        if (ufn > 0)
            llm_stream_send_all(fd, usage_frame, (size_t)ufn);
        llm_response_free(resp);
    }

    AIRY_FREE((void *)cfg.model);
    request_context_destroy(ctx);
}

/* RPC 蹦床：m_<method> ↔ .manifest rpc.methods，壳体经 DAEMON_RPC_SHELL
 * 单点生成；调用点行尾不加分号（宏体自带 '}' 收尾）。壳置于 handler 之后，
 * 原型由生成头 svc_llm_d.h 提供，本文件不再重复前置声明。 */
DAEMON_RPC_SHELL(complete, handle_complete)
DAEMON_RPC_SHELL(complete_stream, handle_complete_stream)
DAEMON_RPC_SHELL(list_models, handle_list_models)
DAEMON_RPC_SHELL(count_tokens, handle_count_tokens)
DAEMON_RPC_SHELL(health_check, handle_health_check)
DAEMON_RPC_SHELL(get_stats, handle_get_stats)
DAEMON_RPC_SHELL(embeddings, handle_embeddings)