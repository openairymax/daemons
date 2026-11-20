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
#include "err_class.h"
#include "error.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"
#include "airy_prov/prov_diag.h"
#include "airy_prov/prov_breaker.h"
#include "airy_prov/provider_http.h"
#include "airy_prov/secrets.h"
#include "response.h"
#include "router/core/llm_rtr_health.h"
#include "token.h"
#include "token_standard.h"

#include <cjson/cJSON.h>

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

/* R-5：complete 总耗时须严格小于调用方背压（0.1.21 C-21 起由预算树保证：
 * 调用方取 server / client 层，本层取 provider 层，且 provider ≤ server）。
 * 否则调用方先放弃，provider 侧精确诊断（鉴权/限流/连接）回传不到用户。
 * 最坏总耗时 = LLM_MAX_RETRIES × 本上界 + 单次超时 = 3 × 5s + 70s = 85s
 * < server 层 150s；单次尝试已接近完整超时（对端不可达/挂起）时不再重试。 */
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
        return "No LLM provider loaded from config/model.yaml (file missing, unreadable, or "
               "declares no providers)";
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

/* C-19：槽位读视图（只读，任何调用方均可取）。 */
static void handle_slots_get(cJSON *params, int id, airy_sock_t fd)
{
    (void)params;
    if (!g_service) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Service not ready", id);
        return;
    }
    send_service_json(llm_slots_get(g_service), id, fd, "Out of memory",
                      "Invalid slots result");
}

/* C-19：槽位绑定。role/model 二者必填（model 允许空串=清空该角色），
 * 校验与持久化在 service 域，成功后回读生效视图，调用方无需再发 get。 */
static void handle_slots_set(cJSON *params, int id, airy_sock_t fd)
{
    JSONRPC_REQUIRE(params && g_service, fd, "Invalid params", id);
    cJSON *rj = cJSON_GetObjectItem(params, "role");
    cJSON *mj = cJSON_GetObjectItem(params, "model");
    const char *role = (cJSON_IsString(rj) && rj->valuestring) ? rj->valuestring : NULL;
    const char *model = (cJSON_IsString(mj) && mj->valuestring) ? mj->valuestring : NULL;
    char err[256];
    int rc = llm_slots_set(g_service, role, model, err, sizeof(err));
    if (rc != 0) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, err[0] ? err : "slots_set failed", id);
        return;
    }
    send_service_json(llm_slots_get(g_service), id, fd, "Out of memory",
                      "Invalid slots result");
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

/* D-2：Provider 连通性探测。密钥写入后由 `agentrt key` 立即调用；
 * 判据与业务请求同源（provider_refresh_api_key 热解析 + prov_probe_get
 * 出网），不复用任何旧值，失败给出可判读原因。 */
typedef struct {
    const char *want;
    const provider_t *hit;
    int exact;
} probe_pick_t;

static int probe_walk(const provider_t *prov, void *ud)
{
    probe_pick_t *pick = (probe_pick_t *)ud;
    if (!prov || !prov->name)
        return 0;
    if (!pick->hit)
        pick->hit = prov;
    if (pick->want && strcmp(prov->name, pick->want) == 0) {
        pick->hit = prov;
        pick->exact = 1;
        return 1;
    }
    return 0;
}

static const char *probe_reason(int rc, long code, int key_missing, int base_missing)
{
    if (base_missing)
        return "api_base_missing";
    if (key_missing)
        return "api_key_missing";
    if (rc != AIRY_OK && code == 0)
        return (rc == AIRY_ERR_TIMEOUT) ? "timeout" : "unreachable";
    if (code >= 200 && code < 400)
        return "ok";
    if (code == 401 || code == 403)
        return "auth_failed";
    if (code == 404)
        return "endpoint_not_found";
    if (code == 429)
        return "rate_limited";
    if (code >= 500)
        return "upstream_error";
    return "unexpected_status";
}

/* 探测目标：显式 model 精确命中优先；未注册的 model 是请求错误，不回落。
 * 否则按 provider 名选（want 为空时取注册表首项）。 */
static const provider_t *probe_pick_prov(cJSON *params, airy_sock_t fd, int id)
{
    cJSON *pj = cJSON_GetObjectItem(params, "provider");
    cJSON *mj = cJSON_GetObjectItem(params, "model");
    const char *want = (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0])
                           ? pj->valuestring
                           : NULL;
    const char *model = (cJSON_IsString(mj) && mj->valuestring && mj->valuestring[0])
                            ? mj->valuestring
                            : NULL;

    provider_registry_t *reg = llm_svc_reg(g_service);
    const provider_t *prov = (model && reg) ? provider_registry_find(reg, model) : NULL;
    if (prov)
        return prov;
    if (model) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "model not registered", id);
        return NULL;
    }

    probe_pick_t pick = {.want = want, .hit = NULL, .exact = 0};
    provider_reg_walk(reg, probe_walk, &pick);
    if (want && !pick.exact) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "provider not found", id);
        return NULL;
    }
    if (!pick.hit) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "no provider configured", id);
        return NULL;
    }
    return pick.hit;
}

/* 传输探测：api_base/密钥齐全才发 GET /models（缺项不发请求，
 * 由 reason 说明缺什么，避免把配置缺失误报成上游不可达）。 */
static int probe_run(const provider_t *prov, provider_base_ctx_t *base, int attempted,
                     long *out_code, double *out_latency)
{
    *out_code = 0;
    *out_latency = 0.0;
    if (!attempted)
        return AIRY_ERR_INVALID_PARAM;
    uint64_t t0 = airy_time_ms();
    provider_http_resp_t *resp = NULL;
    int rc = prov_probe_get(base, "/models", prov->adapter ? prov->adapter->headers : NULL, 8.0,
                            &resp, out_code);
    *out_latency = (double)(airy_time_ms() - t0);
    provider_http_resp_free(resp);
    return rc;
}

/* 探测结果：reachable 只由 reason 判定；实际发过请求且失败时附原始码。 */
static void probe_reply(airy_sock_t fd, int id, const provider_t *prov,
                        const provider_base_ctx_t *base, const char *reason, long code,
                        double latency_ms, int rc, int attempted)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "provider", prov->name);
    cJSON_AddStringToObject(result, "api_base", base->api_base);
    cJSON_AddBoolToObject(result, "reachable", strcmp(reason, "ok") == 0);
    cJSON_AddNumberToObject(result, "http_code", (double)code);
    cJSON_AddNumberToObject(result, "latency_ms", latency_ms);
    cJSON_AddStringToObject(result, "reason", reason);
    if (attempted && rc != AIRY_OK)
        cJSON_AddNumberToObject(result, "rc", (double)rc);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_probe(cJSON *params, int id, airy_sock_t fd)
{
    JSONRPC_REQUIRE(params && g_service, fd, "Invalid params", id);

    const provider_t *prov = probe_pick_prov(params, fd, id);
    if (!prov)
        return;

    provider_base_ctx_t *base = prov->ctx ? provider_base_ctx(prov->ctx) : NULL;
    if (!base) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "provider base unavailable", id);
        return;
    }

    /* 热解析：secrets.env 刚写入的密钥本次即可生效，无需重启。 */
    provider_refresh_api_key(base);

    int base_missing = (base->api_base[0] == '\0');
    int key_missing = (!base_missing && prov->adapter && prov->adapter->headers &&
                       prov->adapter->headers->auth != PROVIDER_AUTH_NONE &&
                       base->api_key[0] == '\0');
    int attempted = !base_missing && !key_missing;

    long code = 0;
    double latency_ms = 0.0;
    int rc = probe_run(prov, base, attempted, &code, &latency_ms);
    probe_reply(fd, id, prov, base, probe_reason(rc, code, key_missing, base_missing), code,
                latency_ms, rc, attempted);
}

static void handle_health_check(cJSON *params, int id, airy_sock_t fd)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    /* 健康口径（0.1.20 D2-5）：进程存活 ≠ 上游可用。唯一判据是端点健康
     * 状态机（探测与请求实测共喂三态），不以句柄存在性冒充健康（G54）。 */
    llm_health_snap_t snap;
    llm_health_state_t st = llm_rtr_health(&snap);

    cJSON_AddStringToObject(result, "service", "llm_d");
    cJSON_AddStringToObject(result, "health", llm_health_name(st));
    cJSON_AddBoolToObject(result, "healthy", st == LLM_HEALTH_OK);
    cJSON_AddNumberToObject(result, "upstream_total", (double)snap.total);
    cJSON_AddNumberToObject(result, "upstream_ok", (double)snap.ok);
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

    /* B-4 调用侧熔断视图：按 provider 聚合并出状态与分口径计数。与端点
     * 健康并置而非合并——前者是"熔断闸开没开"，后者是"端点此刻可不可达"，
     * 两者共同回答"上层失败时是端点挂了还是闸门已跳"。 */
    char *bj = prov_brk_json();
    if (bj) {
        cJSON *breakers = cJSON_Parse(bj);
        AIRY_FREE(bj);
        if (breakers)
            cJSON_AddItemToObject(result, "breakers", breakers);
    }
    /* 单口径读数（C-3）：熔断拒绝 + 预算终止。与 governance 的
     * circuit_breaks 同名同义，CLI/GRAD 不必再从 breaker 数组求和。 */
    cJSON_AddNumberToObject(result, "circuit_breaks", (double)prov_brk_breaks());

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
    if (parse_params(params, ctx, &cfg, 0) != 0) {
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
    /* 对端关闭后置位：后续 chunk 直接拒收，handler 据此不再回写错误帧。 */
    int canceled;
} llm_stream_ctx_t;

#ifndef _WIN32
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#endif

/* 阻塞式全量发送（仅流式）：curl write 回调的推速可超过网关 SSE 拉速，
 * airy_sock_send 的 MSG_DONTWAIT 在缓冲区满时返回 EAGAIN，会让回调失败并
 * 中断流。此处改为 send 循环 + poll(POLLOUT) 等待可写；对端关闭
 * （EPIPE/ECONNRESET）或等待可写超时时返回 -1，由调用方置取消位并让取消
 * 沿 provider → 上游连接逐层传播，避免对已消失的客户端继续生成 token。 */
static int llm_stream_send_all(airy_sock_t fd, const char *buf, size_t len)
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
                return -1;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {

                struct pollfd pfd = {.fd = (int)fd, .events = POLLOUT};
                if (poll(&pfd, 1, 1000) <= 0)
                    return -1;
                continue;
            }
            if (errno == EINTR)
                continue;
            return -1;
#endif
        } else {
            return -1;
        }
    }
    return 0;
}

static int llm_stream_callback(const char *chunk, void *user_data)
{
    llm_stream_ctx_t *sctx = (llm_stream_ctx_t *)user_data;

    if (sctx->canceled)
        return 1;

    /* D5：首个 chunk 到达即首 token 延迟；冷加载只观测不阻断，阈值判定
     * 归机制件（避免常量双写）。 */
    if (sctx->first_ms == 0 && sctx->start_ms != 0) {
        sctx->first_ms = airy_time_ms() - sctx->start_ms;
        ep_health_cold(g_ep, sctx->provider, sctx->model, (double)sctx->first_ms);
    }

    if (llm_stream_send_all(sctx->fd, chunk, strlen(chunk)) != 0) {
        sctx->canceled = 1;
        return 1;
    }
    return 0;
}

/* P24（0.1.12）：流式路径的错误必须以 RS 'E' 帧下发——裸 JSON-RPC 信封会被
 * adapter 流解复用器当正文转发给客户端（社区 ubuntu v0.1.11 实证）；旧
 * adapter 不识别 'E' 标签则整帧丢弃（可接受的降级）。帧体由 cJSON 构造，
 * 避免把 message 以 %s 裸拼进 JSON 时转义责任落到各调用点。
 *
 * '\x1e' 必须写成字符字面量：\x 转义会吞掉紧随的十六进制字符（'E' 也是
 * hex digit），"\x1eE{..." 被解析成 0x1EE（clang #132 macOS 实证）。
 *
 * L-8（0.1.21）：信封 code 只能是 JSON-RPC 协议码，真实上游错误码另置
 * error.data.err（ErrorModel v1：class + code）。此前一律折叠成 -32603，
 * 客户端无法区分「上游不可达 / 上游瞬时故障 / 配置错误」，只能把全部失败
 * 归为不可重试的业务错误。帧体用堆缓冲：失败文案最长 640 字节，固定 512
 * 栈缓冲会让超长帧被静默丢弃，客户端只看得到空流。 */
static void llm_stream_send_error_frame(airy_sock_t fd, int id, int code, int upstream,
                                        const char *message)
{
    cJSON *idj;
    cJSON *data = NULL;
    char *body;
    char *frame;
    size_t blen;

    if (upstream != 0) {
        data = cJSON_CreateObject();
        if (data) {
            (void)airy_err_json_put(data, airy_err_class_of((airy_err_t)upstream),
                                    (airy_err_t)upstream, message, NULL);
        }
    }
    idj = cJSON_CreateNumber(id);
    body = jsonrpc_create_error_response(idj, code, message ? message : "", data);
    cJSON_Delete(idj);
    if (!body)
        return;

    blen = strlen(body);
    frame = (char *)AIRY_MALLOC(blen + 3);
    if (frame) {
        frame[0] = '\x1e';
        frame[1] = 'E';
        AIRY_MEMCPY(frame + 2, body, blen);
        frame[blen + 2] = '\x1e';
        (void)llm_stream_send_all(fd, frame, blen + 3);
        AIRY_FREE(frame);
    }
    AIRY_FREE(body);
}

/* usage 控制帧（RS 'U'）：帧体由 resp_usage_json 单点产出，与非流式共用
 * 同一序列化实现；RS 在运行期拼装——'\x1e' 与后续字符写成相邻字面量时，
 * \x 转义会把该字符按十六进制吞掉。 */
static void send_usage(airy_sock_t fd, const llm_response_t *resp)
{
    static const char head[2] = {'\x1e', 'U'};
    char *body = resp_usage_json(resp);

    if (!body) {
        SVC_LOG_WARN("complete_stream: usage frame body build failed");
        return;
    }
    llm_stream_send_all(fd, head, sizeof(head));
    llm_stream_send_all(fd, body, strlen(body));
    llm_stream_send_all(fd, "\x1e", 1);
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
    if (parse_params(params, ctx, &cfg, PARSE_F_STREAM) != 0) {
        /* 流协议只认 RS 帧：错误走 'E' 帧，不回文本响应。 */
        const char *reason = (ctx && ctx->fail_reason[0]) ? ctx->fail_reason
                                                          : "request params rejected";
        char rbuf[200];
        snprintf(rbuf, sizeof(rbuf), "Invalid params: %s", reason);
        SVC_LOG_ERROR("complete_stream: %s", rbuf);
        llm_stream_send_error_frame(fd, id, JSONRPC_INVALID_PARAMS, 0, rbuf);
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
        /* 取消不是失败：对端已关闭，回写 'E' 帧只会再产生一次 EPIPE，
         * 且日志不应把用户主动中止记成服务错误。 */
        if (stream_ctx.canceled || ret == AIRY_ERR_CANCELED) {
            SVC_LOG_INFO("complete_stream: client disconnected, upstream canceled (id=%d)", id);
            llm_response_free(resp);
            return;
        }
        /* 与 parse_params 失败路径同因同治：错误只能走 RS 'E' 帧。 */
        char ebuf[640];
        const char *msg = llm_fail_msg(ret, ebuf, sizeof(ebuf));
        SVC_LOG_ERROR("complete_stream: %s", msg);
        llm_stream_send_error_frame(fd, id, JSONRPC_INTERNAL_ERROR, ret, msg);
        return;
    }

    if (resp) {
        /* 流式结束发 usage 控制帧（RS 'U'）：携带真实 token、回填后的
         * cost_usd、C11 的 finish_reason、D2-9 的容量回显与分项耗时。帧体
         * 由 rpc/response.c 单点产出——此前手搓 snprintf 与非流式各走一条
         * 通道，timings 只补了 cJSON 那条，流式终帧长期缺这段。 */
        send_usage(fd, resp);
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
DAEMON_RPC_SHELL(slots_get, handle_slots_get)
DAEMON_RPC_SHELL(slots_set, handle_slots_set)
DAEMON_RPC_SHELL(count_tokens, handle_count_tokens)
DAEMON_RPC_SHELL(probe, handle_probe)
DAEMON_RPC_SHELL(health_check, handle_health_check)
DAEMON_RPC_SHELL(get_stats, handle_get_stats)
DAEMON_RPC_SHELL(embeddings, handle_embeddings)