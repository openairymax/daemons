// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_request.c
 * @brief LLM service request-handling domain: cache-key generation,
 *        provider routing selection, full/streaming completion handling
 *        and cost tracking.
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "response.h"
#include "router/llm_router.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpc/internal.h"

/* 语义缓存（mem_d）RPC 超时：缓存是可选加速层，本地 socket 往返毫秒级，
 * 此处仅需给出上界以保证 mem_d 繁忙时不拖慢主链路（13-semantic-cache
 * §3.5：缓存引擎异常必须降级为直连 LLM）。 */
#define LLM_MEM_CACHE_TIMEOUT_MS 2000

/**
 * @brief 构造请求的规范化文本（canonical text）：
 *        "role:content:reasoning|" 逐条拼接。
 *
 * 该文本同时是本地 LRU 键与 mem_d 语义缓存 L0 键（sha256(text + "|"
 * + model_id)）的输入，保证两级缓存对"同一请求"的判定一致。
 *
 * @return 文本串（调用方 AIRY_FREE），失败返回 NULL
 */
static char *make_cache_text(const llm_request_config_t *manager)
{
    if (!manager) {
        return NULL;
    }

    size_t len = 1;
    for (size_t i = 0; i < manager->message_count; ++i) {
        const char *role = manager->messages[i].role ? manager->messages[i].role : "";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        const char *reasoning =
            manager->messages[i].reasoning_content ? manager->messages[i].reasoning_content : "";
        len += strlen(role) + 1 + strlen(content) + 1 + strlen(reasoning) + 1;
    }

    char *text = (char *)AIRY_MALLOC(len);
    if (!text) {
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; i < manager->message_count; ++i) {
        const char *role = manager->messages[i].role ? manager->messages[i].role : "";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        const char *reasoning =
            manager->messages[i].reasoning_content ? manager->messages[i].reasoning_content : "";

        int written = snprintf(text + pos, len - pos, "%s:%s:%s|", role, content, reasoning);
        if (written < 0 || (size_t)written >= len - pos) {
            pos = len - 1;
            break;
        }
        pos += (size_t)written;
    }

    text[pos < len ? pos : len - 1] = '\0';
    return text;
}

/**
 * @brief 生成本地 LRU 缓存键："<model>|<canonical text>"
 * @param manager 请求配置
 * @param text    make_cache_text() 的输出
 * @return 键串（调用方 AIRY_FREE），失败返回 NULL
 */
static char *make_cache_key(const llm_request_config_t *manager, const char *text)
{
    if (!manager || !manager->model || !text) {
        return NULL;
    }

    size_t len = strlen(manager->model) + 1 + strlen(text) + 1;
    char *key = (char *)AIRY_MALLOC(len);
    if (!key) {
        return NULL;
    }

    snprintf(key, len, "%s|%s", manager->model, text);
    return key;
}

/* ── mem_d 跨进程语义缓存（A-IPC over Unix socket） ────────────────────
 *
 * 命中判定与存储都走 mem_d 的 cache_get / cache_put 方法（mem.sock）。
 * 缓存是可选加速层：任何失败（daemon 未启动、超时、协议错误、g_cache
 * 未初始化）一律按"未命中/未写入"处理，主流程继续直连上游 LLM，仅以
 * DEBUG 记录，避免 mem_d 缺席时每次请求刷屏。
 */

static const char *mem_sock_path(void)
{
    const char *sock = airy_runtime_dir_socket("mem.sock");
    return (sock && sock[0]) ? sock : NULL;
}

/**
 * @brief 查询 mem_d 语义缓存
 * @return 1 命中（*out_json 为响应 JSON，调用方 AIRY_FREE）；0 未命中或不可用
 */
static int mem_cache_fetch(const char *text, const char *model, char **out_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !out_json) {
        return 0;
    }
    *out_json = NULL;

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return 0;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "model_id", model);
    char *params = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!params) {
        return 0;
    }

    char *result = NULL;
    int rc = daemon_rpc_call(sock, "cache_get", params, &result, LLM_MEM_CACHE_TIMEOUT_MS);
    AIRY_FREE(params);
    if (rc != AIRY_SUCCESS || !result) {
        SVC_LOG_DEBUG("mem.cache_get unavailable (rc=%d) — semantic cache bypassed", rc);
        AIRY_FREE(result);
        return 0;
    }

    int hit = 0;
    cJSON *root = cJSON_Parse(result);
    AIRY_FREE(result);
    if (!root) {
        return 0;
    }

    const cJSON *hit_item = cJSON_GetObjectItem(root, "hit");
    const cJSON *body_item = cJSON_GetObjectItem(root, "response");
    if (cJSON_IsBool(hit_item) && cJSON_IsTrue(hit_item) && cJSON_IsString(body_item) &&
        body_item->valuestring && body_item->valuestring[0]) {
        *out_json = AIRY_STRDUP(body_item->valuestring);
        hit = (*out_json != NULL);
    }
    cJSON_Delete(root);
    return hit;
}

/**
 * @brief 写入 mem_d 语义缓存（失败仅 DEBUG，不阻断主流程）
 */
static void mem_cache_save(const char *text, const char *model, const char *resp_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !resp_json) {
        return;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "response", resp_json);
    cJSON_AddStringToObject(req, "model_id", model);
    /* B5-3：仅在 cache_store 准入门禁通过后才会到达此处，故显式声明可缓存 */
    cJSON_AddBoolToObject(req, "cacheable", 1);
    char *params = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!params) {
        return;
    }

    char *result = NULL;
    int rc = daemon_rpc_call(sock, "cache_put", params, &result, LLM_MEM_CACHE_TIMEOUT_MS);
    AIRY_FREE(params);
    AIRY_FREE(result);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_DEBUG("mem.cache_put unavailable (rc=%d) — semantic cache not updated", rc);
    }
}

/**
 * @brief 两级缓存查询：L0 进程内精确匹配 LRU → L1 mem_d 语义缓存。
 *        L1 命中后回填 L0，后续同请求免 IPC。
 * @return 1 命中（*out_response 已填充）；0 未命中
 */
static int cache_lookup(llm_service_t *svc, const char *key, const char *text, const char *model,
                        llm_response_t **out_response)
{
    if (!svc || !key || !text || !model || !out_response) {
        SVC_LOG_ERROR("cache_lookup: NULL parameter (svc=%p, key=%p, text=%p, model=%p, out=%p)",
                      (const void *)svc, (const void *)key, (const void *)text, (const void *)model,
                      (const void *)out_response);
        return 0;
    }

    char *cached_json = NULL;
    if (llm_cache_get(svc->cache, key, &cached_json) == 1 && cached_json) {
        llm_response_t *local_resp = response_from_json(cached_json);
        if (local_resp) {
            AIRY_FREE(cached_json);
            *out_response = local_resp;
            SVC_LOG_DEBUG("L0 cache hit");
            return 1;
        }
        SVC_LOG_WARN("L0 cached response unparsable, falling back to semantic cache");
    }
    AIRY_FREE(cached_json);

    char *sem_json = NULL;
    if (mem_cache_fetch(text, model, &sem_json) != 1 || !sem_json) {
        AIRY_FREE(sem_json);
        return 0;
    }

    llm_response_t *sem_resp = response_from_json(sem_json);
    if (sem_resp) {
        llm_cache_put(svc->cache, key, sem_json);
        *out_response = sem_resp;
        SVC_LOG_DEBUG("L1 semantic cache hit (promoted to L0)");
    }
    AIRY_FREE(sem_json);
    return sem_resp ? 1 : 0;
}

/**
 * @brief Find a provider
 */
static const provider_t *find_provider(llm_service_t *svc, const char *model)
{
    if (!svc || !model) {
        SVC_LOG_ERROR("find_provider: NULL parameter (svc=%p, model=%p)", (const void *)svc,
                      (const void *)model);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    const provider_t *prov = provider_registry_find(svc->registry, model);
    airy_mtx_unlock(&svc->lock);

    return prov;
}

/* ---------- P3.16 (ACC-DT17): select provider via llm_router ----------
 *
 * Explicit model wins (GRAD three-model separation):
 *   When the caller explicitly specifies a model (e.g. t2=glm-4,
 *   t1-f=deepseek-flash) and it matches exactly in the registry, return that
 *   provider directly — COST_AWARE routing would pick the "cheapest endpoint"
 *   and may ignore the user-specified model (the risk of t2 being routed to
 *   deepseek). Only when the exact match fails, fall back to strategy routing.
 *
 * On routing failure (e.g. not initialized, no endpoint matching the
 * capabilities, empty registry) return NULL, and the caller falls back to
 * find_provider(model) for backward compatibility. */
static const provider_t *select_provider_via_router(llm_service_t *svc,
                                                    const llm_request_config_t *manager,
                                                    bool is_stream)
{
    if (!svc || !manager) {
        return NULL;
    }

    if (manager->model && manager->model[0]) {
        const provider_t *exact = find_provider(svc, manager->model);
        if (exact) {
            SVC_LOG_INFO("C-L02: SVC: explicit model %s resolved directly "
                         "(provider=%s, skip router)",
                         manager->model, exact->name ? exact->name : "?");
            return exact;
        }
        SVC_LOG_WARN("C-L02: SVC: explicit model %s not in registry (simplified "
                     "llm section exposes one model only), falling back to router — "
                     "think section models outside the registry are served by the "
                     "default model",
                     manager->model ? manager->model : "(null)");
    }

    llm_route_request_t req;
    __builtin_memset(&req, 0, sizeof(req));

    if (manager->message_count > 0 && manager->messages && manager->messages[0].content) {
        req.prompt = manager->messages[0].content;
        req.prompt_len = strlen(req.prompt);
    } else {
        req.prompt = "";
        req.prompt_len = 0;
    }

    req.required_caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    if (is_stream) {
        req.required_caps |= LLM_CAP_STREAMING;
    }

    /* 成本估算用输出量：调用方未指定时用引擎默认上限，使路由估算与最终
     * 实际发出的 max_tokens 同源（此前恒为 0，估算与实际脱节）。 */
    {
        int est = (manager->max_tokens > 0) ? manager->max_tokens : svc->default_max_output_tokens;
        req.max_tokens = (est > 0) ? (uint32_t)est : 0;
    }
    req.max_cost = 0;
    req.max_latency_ms = 0;
    req.strategy = LLM_ROUTE_COST;
    req.preferred_provider[0] = '\0';

    llm_route_result_t result;
    __builtin_memset(&result, 0, sizeof(result));
    int rc = llm_router_route(&req, &result);
    if (rc != 0) {
        SVC_LOG_DEBUG("C-L02: SVC: router_route rc=%d — will fall back to find_provider(%s)", rc,
                      manager->model ? manager->model : "NULL");
        return NULL;
    }

    /* Resolve the routed model_name to the real provider via the registry.
     * If the routed model does not exist in the registry (theoretically
     * impossible since endpoints originate from the registry), return NULL to
     * trigger the caller's fallback. */
    const provider_t *prov = find_provider(svc, result.model_name);
    if (prov) {
        SVC_LOG_INFO("C-L02: SVC: ROUTED provider=%s model=%s strategy=%d cost=%.6f latency=%u",
                     result.provider_name, result.model_name, (int)result.strategy_used,
                     result.estimated_cost, result.estimated_latency_ms);
    }
    return prov;
}

/**
 * @brief 两级写入：L0 进程内 LRU + L1 mem_d 语义缓存
 *
 * B5-3 准入门禁（fail-closed）：调用方未显式声明 cacheable 时两级均不写入，
 * 含敏感面的请求由 mem_d 侧 mem_cache_admit() 二次拒绝。
 */
static void cache_store(llm_service_t *svc, const char *key, const char *text, const char *model,
                        llm_response_t *resp, int cacheable)
{
    if (!svc || !key || !text || !model || !resp) {
        return;
    }
    if (!cacheable) {
        return;
    }

    char *resp_json = response_to_json(resp);
    if (!resp_json) {
        return;
    }

    llm_cache_put(svc->cache, key, resp_json);
    mem_cache_save(text, model, resp_json);
    AIRY_FREE(resp_json);
}

/**
 * @brief 生成参数唯一解析点：把"用哪个模型、最多生成多少 token"从三处来源
 *        （调用方显式值 / 注册表中该模型声明的上限 / 引擎默认上限）收敛为
 *        一次判定，同步与流式两条路径共用，避免各自解析造成口径漂移。
 *
 * 显式 max_tokens 是意图，模型上限与引擎默认是边界：意图超过边界按边界截断
 * （配置写了上限就必须兑现），未声明意图时取边界，边界也未配置则保持未设。
 */
void resolve_gen_params(const llm_service_t *svc, const provider_t *prov,
                        const llm_request_config_t *manager, char *out_model, size_t model_size,
                        int *out_max_tokens)
{
    const char *model = (manager->model && manager->model[0]) ? manager->model : svc->default_model;
    if (!model || !model[0])
        model = "";

    int limit = provider_registry_model_max_output(prov, model);
    if (limit <= 0)
        limit = svc->default_max_output_tokens;

    int want = manager->max_tokens;
    int resolved = (limit > 0 && (want <= 0 || want > limit)) ? limit : want;
    if (resolved < 0)
        resolved = 0;

    AIRY_STRNCPY_TERM(out_model, model, model_size);
    *out_max_tokens = resolved;

    if (want > 0 && limit > 0 && want > limit) {
        SVC_LOG_WARN("C-L02: SVC: max_tokens=%d exceeds the cap (%d) of model=%s — clipped", want,
                     limit, model);
    } else if (resolved > 0) {
        SVC_LOG_INFO("C-L02: SVC: max_tokens=%d resolved for model=%s (explicit=%d, cap=%d)",
                     resolved, model, want, limit);
    }
}

/* 0.1.17：上游因达到输出上限而截断时必须可判读。此前 finish_reason 只被透传，
 * 引擎不做判定，用户看到"回答莫名中断"却无任何线索。 */
static void note_truncation(const llm_response_t *resp, const char *model, int max_tokens)
{
    if (resp && llm_finish_is_truncated(resp->finish_reason)) {
        SVC_LOG_WARN("C-L02: SVC: output truncated at max_tokens=%d (model=%s) — raise the model "
                     "max_output in model.yaml or shorten the request",
                     max_tokens, model ? model : "?");
    }
}

/**
 * @brief Update cost tracking (accumulate + fill per-call cost back into the
 *        response)
 */
static void update_cost_tracking(llm_service_t *svc, const char *model, llm_response_t *resp)
{
    if (!svc || !model || !resp) {
        return;
    }

    cost_tracker_add(svc->cost, model, resp->prompt_tokens, resp->completion_tokens);
    resp->cost_usd =
        cost_tracker_estimate(svc->cost, model, resp->prompt_tokens, resp->completion_tokens);

    /* 2.1.1.5 修复：每次真实调用后兜底持久化——防 daemon 异常退出
     * （SIGKILL/崩溃）丢失本次累计（优雅退出路径在 llm_service_destroy
     * 再保存一次，幂等）。 */
    const char *usage_path = llm_usage_state_path();
    if (usage_path)
        (void)cost_tracker_save(svc->cost, usage_path);
}

int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response)
{

    if (!svc || !manager || !out_response) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL invalid arguments, STACK: llm_service_complete");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!manager->model) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL model=NULL, STACK: llm_service_complete");
        return AIRY_ERR_INVALID_PARAM;
    }

    char *cache_text = make_cache_text(manager);
    char *cache_key = cache_text ? make_cache_key(manager, cache_text) : NULL;
    if (!cache_text || !cache_key) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL cache key alloc, STACK: llm_service_complete");
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    llm_response_t *cached_resp = NULL;
    int cache_status = cache_lookup(svc, cache_key, cache_text, manager->model, &cached_resp);
    if (cache_status > 0 && cached_resp) {
        /* 2.1.1.5 修复：缓存命中同样计入 cost_tracker——此前命中直接
         * 返回跳过计费，累计金额低于逐轮显示之和（缓存响应仍消耗上游
         * token 配额，只是本侧免去一次转发）。按缓存响应自带的 token
         * 数计费，金额语义一致。 */
        update_cost_tracking(svc, manager->model, cached_resp);
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        *out_response = cached_resp;
        return AIRY_OK;
    }

    /* P3.16 (ACC-DT17): prefer strategy routing via llm_router to select the
     * provider; on routing failure (not initialized / no capability-matching
     * endpoint / empty registry) fall back to find_provider(manager->model)
     * for backward compatibility. */
    const provider_t *prov = select_provider_via_router(svc, manager, false);
    if (!prov) {
        SVC_LOG_INFO("C-L02: SVC: router miss — falling back to find_provider(model=%s)",
                     manager->model);
        prov = find_provider(svc, manager->model);
    }
    if (!prov) {
        SVC_LOG_ERROR(
            "C-L02: SVC: COMPLETE-FAIL model=%s, error=INVALID_MODEL, STACK: llm_service_complete",
            manager->model);
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        cache_key = NULL;
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    {

        const char *first_content = NULL;
        size_t input_len = 0;
        if (manager->message_count > 0 && manager->messages[0].content) {
            first_content = manager->messages[0].content;
            input_len = strlen(first_content);
        }
        llm_complexity_level_t complexity = assess_complexity(first_content);
        log_routing_decision(manager->model, complexity, input_len, "user_specified");
    }

    llm_request_config_t eff = *manager;
    char eff_model[128];
    int eff_max_tokens = 0;
    resolve_gen_params(svc, prov, manager, eff_model, sizeof(eff_model), &eff_max_tokens);
    eff.model = eff_model;
    eff.max_tokens = eff_max_tokens;

    llm_response_t *resp = NULL;
    int ret = prov->ops->complete(prov->ctx, &eff, &resp);
    if (ret != 0) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL model=%s, error=%d, STACK: llm_service_complete",
                      eff_model, ret);
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        cache_key = NULL;
        return ret;
    }

    note_truncation(resp, eff_model, eff_max_tokens);
    update_cost_tracking(svc, eff_model, resp);
    cache_store(svc, cache_key, cache_text, manager->model, resp, manager->cacheable);

    *out_response = resp;
    AIRY_FREE(cache_text);
    AIRY_FREE(cache_key);
    cache_key = NULL;
    return AIRY_OK;
}

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response)
{

    if (!svc || !manager || !callback) {
        SVC_LOG_ERROR(
            "C-L02: SVC: STREAM-FAIL invalid arguments, STACK: llm_service_complete_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!manager->model) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=NULL, STACK: llm_service_complete_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    /* P3.16 (ACC-DT17): prefer strategy routing via llm_router to select the
     * provider; on routing failure fall back to find_provider(manager->model)
     * for backward compatibility. */
    const provider_t *prov = select_provider_via_router(svc, manager, true);
    if (!prov) {
        SVC_LOG_INFO("C-L02: SVC: router miss (stream) — falling back to find_provider(model=%s)",
                     manager->model);
        prov = find_provider(svc, manager->model);
    }
    if (!prov) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=%s, error=INVALID_MODEL, STACK: "
                      "llm_service_complete_stream",
                      manager->model);
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    {
        const char *first_content = NULL;
        size_t input_len = 0;
        if (manager->message_count > 0 && manager->messages[0].content) {
            first_content = manager->messages[0].content;
            input_len = strlen(first_content);
        }
        llm_complexity_level_t complexity = assess_complexity(first_content);
        log_routing_decision(manager->model, complexity, input_len, "stream_user_specified");
    }

    if (!prov->ops->complete_stream) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=%s, error=NOT_SUPPORTED, STACK: "
                      "llm_service_complete_stream",
                      manager->model);
        return AIRY_ERR_NOT_SUPPORTED;
    }

    llm_request_config_t eff = *manager;
    char eff_model[128];
    int eff_max_tokens = 0;
    resolve_gen_params(svc, prov, manager, eff_model, sizeof(eff_model), &eff_max_tokens);
    eff.model = eff_model;
    eff.max_tokens = eff_max_tokens;

    int ret = prov->ops->complete_stream(prov->ctx, &eff, callback, callback_data, out_response);

    if (ret == 0 && out_response && *out_response) {
        llm_response_t *resp = *out_response;
        note_truncation(resp, eff_model, eff_max_tokens);
        cost_tracker_add(svc->cost, eff_model, resp->prompt_tokens, resp->completion_tokens);
        resp->cost_usd = cost_tracker_estimate(svc->cost, eff_model, resp->prompt_tokens,
                                               resp->completion_tokens);
    }

    return ret;
}

int llm_service_embeddings(llm_service_t *svc, const char *model, const char *request_body,
                           char **out_json)
{
    if (!svc || !request_body || !request_body[0] || !out_json)
        return AIRY_ERR_INVALID_PARAM;
    *out_json = NULL;

    const char *m = (model && model[0]) ? model : svc->default_model;
    const provider_t *prov = find_provider(svc, m ? m : "");
    if (!prov || !prov->ctx) {
        SVC_LOG_ERROR("embeddings: no provider for model=%s", m ? m : "(default)");
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    provider_refresh_api_key(base);

    /* OpenAI 兼容 embeddings 端点：$api_base/embeddings（去掉尾部斜杠防双斜杠） */
    char url[1024];
    size_t blen = strlen(base->api_base);
    while (blen > 0 && (base->api_base[blen - 1] == '/' || base->api_base[blen - 1] == '\\'))
        blen--;
    if (blen == 0) {
        SVC_LOG_ERROR("embeddings: empty api_base for provider=%s", prov->name ? prov->name : "?");
        return AIRY_ERR_INVALID_PARAM;
    }
    snprintf(url, sizeof(url), "%.*s/embeddings", (int)blen, base->api_base);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (base->api_key[0]) {
        char auth[320];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", base->api_key);
        headers = curl_slist_append(headers, auth);
    }

    provider_http_resp_t *resp = NULL;
    long http_code = 0;
    int rc = provider_http_post(url, headers, request_body, base->timeout_sec, base->max_retries,
                                &resp, &http_code);
    if (headers)
        curl_slist_free_all(headers);

    if (rc != 0 || !resp) {
        provider_http_resp_free(resp);
        SVC_LOG_ERROR("embeddings: HTTP request failed url=%s rc=%d", url, rc);
        return AIRY_ERR_IO;
    }

    if (http_code >= 400) {
        SVC_LOG_WARN("embeddings: upstream HTTP %ld url=%s", http_code, url);
        AIRY_FREE(resp->data);
        AIRY_FREE(resp);
        return AIRY_ERR_IO;
    }

    *out_json = AIRY_STRDUP(resp->data ? resp->data : "{}");
    provider_http_resp_free(resp);
    if (!*out_json)
        return AIRY_ERR_OUT_OF_MEMORY;
    return AIRY_SUCCESS;
}
