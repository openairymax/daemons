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
#include "error.h"
#include "response.h"
#include "router/core/llm_router.h"
#include "semantic_cache.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accounting/metrics.h"
#include "rpc/rpc_internal.h"
#include "providers/core/secrets.h"
#include "providers/core/provider_http.h"

/**
 * @brief 构造规范化文本 "role:content:reasoning|"，同时作为本地 LRU 键与
 *        mem_d 语义缓存 L0 键的输入，保证两级缓存对同一请求判定一致。
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
 * @brief 本地 LRU 键 "<model>|<canonical text>"（调用方 AIRY_FREE）。
 *
 * 模型段取实际服务该请求的模型，而非请求模型：链式切换后同一逻辑请求
 * 可能落在他处，两者答案不可互相冒充（0.1.19 §3.8.4 H8）。
 */
static char *make_cache_key(const char *model, const char *text)
{
    if (!model || !text) {
        return NULL;
    }

    size_t len = strlen(model) + 1 + strlen(text) + 1;
    char *key = (char *)AIRY_MALLOC(len);
    if (!key) {
        return NULL;
    }

    snprintf(key, len, "%s|%s", model, text);
    return key;
}

/**
 * @brief 两级查询：L0 进程内 LRU → L1 mem_d 语义缓存，L1 命中回填 L0。
 *        B5-3 fail-closed：未显式声明 cacheable 时两级均旁路，与
 *        cache_store 写入门禁同源同判。
 * @return 1 命中（*out_response 已填充）；0 未命中
 */
static int cache_lookup(llm_service_t *svc, const char *key, const char *text, const char *model,
                        int cacheable, llm_response_t **out_response)
{
    if (!svc || !key || !text || !model || !out_response) {
        SVC_LOG_ERROR("cache_lookup: NULL parameter (svc=%p, key=%p, text=%p, model=%p, out=%p)",
                      (const void *)svc, (const void *)key, (const void *)text, (const void *)model,
                      (const void *)out_response);
        return 0;
    }

    if (!cacheable) {
        return 0;
    }

    char *cached_json = NULL;
    if (cache_get_string(svc->cache, key, &cached_json) == 1 && cached_json) {
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
    if (llm_semantic_cache_fetch(text, model, &sem_json) != 1 || !sem_json) {
        AIRY_FREE(sem_json);
        return 0;
    }

    llm_response_t *sem_resp = response_from_json(sem_json);
    if (sem_resp) {
        cache_put_string(svc->cache, key, sem_json);
        *out_response = sem_resp;
        SVC_LOG_DEBUG("L1 semantic cache hit (promoted to L0)");
    }
    AIRY_FREE(sem_json);
    return sem_resp ? 1 : 0;
}

/* 注册表精确查找在 service_chain.c（链式切换同源使用）。 */

/* 候选枚举请求的唯一构造点：首选选路与链式候选共用，避免两处 prompt /
 * caps / max_tokens 口径漂移。 */
static void build_route_request(llm_service_t *svc, const llm_request_config_t *manager,
                                bool is_stream, llm_route_request_t *req)
{
    __builtin_memset(req, 0, sizeof(*req));

    if (manager->message_count > 0 && manager->messages && manager->messages[0].content) {
        req->prompt = manager->messages[0].content;
        req->prompt_len = strlen(req->prompt);
    } else {
        req->prompt = "";
        req->prompt_len = 0;
    }

    req->required_caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    if (is_stream) {
        req->required_caps |= LLM_CAP_STREAMING;
    }

    /* 估算用输出量与最终 max_tokens 同源：未指定时取引擎默认上限。 */
    {
        int est = (manager->max_tokens > 0) ? manager->max_tokens : svc->default_max_output_tokens;
        req->max_tokens = (est > 0) ? (uint32_t)est : 0;
    }
    req->max_cost = 0;
    req->max_latency_ms = 0;
    req->strategy = LLM_ROUTE_COST;
    req->preferred_provider[0] = '\0';
}

/* ---------- llm_router 选路 ----------
 * 显式 model 精确命中注册表时直接返回：COST_AWARE 选路会挑"最便宜端点"，
 * 可能把用户指定的模型（GRAD 三模型分离）改写掉。未命中或选路失败才回落
 * 策略路由，最终由 pick_provider 再做注册表兜底。 */
static const provider_t *select_provider_via_router(llm_service_t *svc,
                                                    const llm_request_config_t *manager,
                                                    const llm_route_request_t *req)
{
    if (!svc || !manager || !req) {
        return NULL;
    }

    if (manager->model && manager->model[0]) {
        const provider_t *exact = svc_find_prov(svc, manager->model);
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

    llm_route_result_t result;
    __builtin_memset(&result, 0, sizeof(result));
    int rc = llm_router_route(req, &result);
    if (rc != 0) {
        SVC_LOG_DEBUG("C-L02: SVC: router_route rc=%d — will fall back to registry lookup(%s)", rc,
                      manager->model ? manager->model : "NULL");
        return NULL;
    }

    /* 选路结果回到注册表解析真实 provider；失败则交由调用方兜底。 */
    const provider_t *prov = svc_find_prov(svc, result.model_name);
    if (prov) {
        SVC_LOG_INFO("C-L02: SVC: ROUTED provider=%s model=%s strategy=%d cost=%.6f latency=%u",
                     result.provider_name, result.model_name, (int)result.strategy_used,
                     result.estimated_cost, result.estimated_latency_ms);
    }
    return prov;
}

/* 双入口统一的选取链：策略路由优先，miss 时回落注册表精确查找；
 * 仍无可用 provider 则记录带 STACK 的失败日志并返回 NULL。 */
static const provider_t *pick_provider(llm_service_t *svc, const llm_request_config_t *manager,
                                       const llm_route_request_t *req, const char *stack,
                                       bool is_stream)
{
    const provider_t *prov = select_provider_via_router(svc, manager, req);
    if (!prov) {
        SVC_LOG_INFO("C-L02: SVC: router miss — falling back to registry lookup(model=%s)",
                     manager->model);
        prov = svc_find_prov(svc, manager->model);
    }
    if (!prov)
        SVC_LOG_ERROR("C-L02: SVC: %s model=%s, error=INVALID_MODEL, STACK: %s",
                      is_stream ? "STREAM-FAIL" : "COMPLETE-FAIL", manager->model, stack);
    return prov;
}
/**
 * @brief 两级写入：L0 LRU + L1 mem_d。B5-3 fail-closed：未显式声明
 *        cacheable 时不写；敏感面另由 mem_d 的 mem_cache_admit() 拒绝。
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

    cache_put_string(svc->cache, key, resp_json);
    llm_semantic_cache_save(text, model, resp_json);
    AIRY_FREE(resp_json);
}

/**
 * @brief 生成参数唯一解析点（同步/流式共用）：模型与 max_tokens 从"显式值 /
 *        模型声明上限 / 引擎默认"三处来源收敛为一次判定。显式值是意图，
 *        上限是边界：意图越界按边界截断，未声明意图取边界，均无则保持未设。
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

/* 0.1.17：上游因输出上限截断必须可判读，否则表现为"回答莫名中断"。 */
static void note_truncation(const llm_response_t *resp, const char *model, int max_tokens)
{
    if (resp && llm_finish_is_truncated(resp->finish_reason)) {
        SVC_LOG_WARN("C-L02: SVC: output truncated at max_tokens=%d (model=%s) — raise the model "
                     "max_output in model.yaml or shorten the request",
                     max_tokens, model ? model : "?");
    }
}

/* 累计用量并回填单次成本。持久化走聚合窗口：dirty 且到阈值/间隔才写，
 * 热路径稳态零 I/O；进程退出的同步落盘由 service destroy 负责。 */
static void update_cost_tracking(llm_service_t *svc, const char *model, llm_response_t *resp)
{
    if (!svc || !model || !resp) {
        return;
    }

    cost_tracker_add(svc->cost, model, resp->prompt_tokens, resp->completion_tokens,
                     resp->cached_tokens);
    resp->cost_usd =
        cost_tracker_estimate(svc->cost, model, resp->prompt_tokens, resp->completion_tokens);

    const char *usage_path = llm_usage_state_path();
    if (usage_path)
        (void)cost_tracker_flush(svc->cost, usage_path);
}

/* complete/stream 共用的 routing prelude：复杂度评估与路由决策日志。 */
static void log_routing_prelude(const llm_request_config_t *manager,
                                const char *routing_kind)
{
    const char *first_content = NULL;
    size_t input_len = 0;
    if (manager->message_count > 0 && manager->messages[0].content) {
        first_content = manager->messages[0].content;
        input_len = strlen(first_content);
    }
    llm_complexity_level_t complexity = assess_complexity(first_content);
    log_routing_decision(manager->model, complexity, input_len, routing_kind);
}

/* 漂移探针：请求侧廉价检测（0.1.20 D2-7）。
 * 只检测与上报，不重载——应用唯一入口是 SIGHUP，避免请求线程排空自身
 * （排空要等在途请求归还，本请求取号前发起排空就会把自己等成超时）。
 * 元数据未变时单次 stat 返回，热路径开销可忽略。 */
static void cfg_probe(llm_service_t *svc)
{
    (void)svc_mw_check(&svc->mw);
    (void)svc_mw_check(&svc->mw_usr);
}

/* 取号：排空窗口内的新请求直接判忙，由调用方稍后重试。 */
static int req_enter(llm_service_t *svc, uint32_t *gen, const char *fn)
{
    int rc = svc_swap_enter(&svc->swap, gen);
    if (rc != 0)
        SVC_LOG_WARN("C-L02: SVC: %s rejected — config hot swap in progress, STACK: %s", fn, fn);
    return rc;
}

/* 归还并裁决世代：世代已推进说明本次结果基于旧装载，不得回流给调用方。 */
static int req_leave(llm_service_t *svc, uint32_t gen, llm_response_t **out)
{
    if (svc_swap_leave(&svc->swap, gen))
        return 0;
    if (out && *out) {
        llm_response_free(*out);
        *out = NULL;
    }
    SVC_LOG_WARN("C-L02: SVC: response dropped — model generation advanced mid-request");
    return 1;
}

/* Shared entry guard: parameter shape and explicit model. */
static int req_guard(llm_service_t *svc, const llm_request_config_t *manager,
                     int sink_ok, const char *tag, const char *fn)
{
    if (!svc || !manager || !sink_ok) {
        SVC_LOG_ERROR("C-L02: SVC: %s-FAIL invalid arguments, STACK: %s", tag, fn);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!manager->model) {
        SVC_LOG_ERROR("C-L02: SVC: %s-FAIL model=NULL, STACK: %s", tag, fn);
        return AIRY_ERR_INVALID_PARAM;
    }
    return AIRY_OK;
}

/* complete 真实调用闭包：适配器直调，机制层不感知传输。 */
static int complete_attempt(void *ctx, const svc_step_t *step, llm_response_t **out)
{
    (void)ctx;
    int ret = step->prov->adapter->complete(step->prov->ctx, &step->eff, out);
    if (ret != 0)
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-ATTEMPT-FAIL provider=%s model=%s error=%d",
                      step->prov->name ? step->prov->name : "?", step->eff.model, ret);
    return ret;
}

/* 流式首字节哨兵：任一带内容的块下发即视为已提交，此后禁止切换
 * （0.1.19 §3.8.4 H6：两段不相接的内容不可拼接给下游）。 */
typedef struct {
    llm_stream_callback_t cb;
    void *ud;
    int sent;
} stream_ctx_t;

static void stream_guard(const char *chunk, void *user_data)
{
    stream_ctx_t *ctx = (stream_ctx_t *)user_data;
    if (chunk && chunk[0])
        ctx->sent = 1;
    ctx->cb(chunk, ctx->ud);
}

static int stream_sent(void *ctx)
{
    return ((stream_ctx_t *)ctx)->sent;
}

static int stream_attempt(void *ctx, const svc_step_t *step, llm_response_t **out)
{
    stream_ctx_t *sc = (stream_ctx_t *)ctx;

    if (!step->prov->adapter->complete_stream) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-ATTEMPT-FAIL provider=%s model=%s error=NOT_SUPPORTED",
                      step->prov->name ? step->prov->name : "?", step->eff.model);
        return AIRY_ERR_NOT_SUPPORTED;
    }

    int ret = step->prov->adapter->complete_stream(step->prov->ctx, &step->eff, stream_guard, sc,
                                                   out);
    if (ret != 0)
        SVC_LOG_ERROR("C-L02: SVC: STREAM-ATTEMPT-FAIL provider=%s model=%s error=%d",
                      step->prov->name ? step->prov->name : "?", step->eff.model, ret);
    return ret;
}

static int complete_impl(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response)
{
    int grc = req_guard(svc, manager, out_response != NULL, "COMPLETE", "llm_service_complete");
    if (grc != AIRY_OK)
        return grc;

    char *cache_text = make_cache_text(manager);
    char *cache_key = cache_text ? make_cache_key(manager->model, cache_text) : NULL;
    if (!cache_text || !cache_key) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL cache key alloc, STACK: llm_service_complete");
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    llm_response_t *cached_resp = NULL;
    int cache_status =
        cache_lookup(svc, cache_key, cache_text, manager->model, manager->cacheable, &cached_resp);
    if (cache_status > 0 && cached_resp) {
        /* 缓存命中仍消耗上游 token 配额，按响应自带用量计费。 */
        update_cost_tracking(svc, manager->model, cached_resp);
        (void)llm_rtr_meas(NULL, NULL, cached_resp, 0);
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        *out_response = cached_resp;
        return AIRY_OK;
    }

    llm_route_request_t req;
    build_route_request(svc, manager, false, &req);

    const provider_t *prov = pick_provider(svc, manager, &req, "llm_service_complete", false);
    if (!prov) {
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    log_routing_prelude(manager, "user_specified");

    svc_hit_t hit;
    __builtin_memset(&hit, 0, sizeof(hit));
    llm_response_t *resp = NULL;
    int ret = svc_chain_run(svc, manager, &req, prov, complete_attempt, NULL, NULL, &resp, &hit);
    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL model=%s, error=%d, STACK: llm_service_complete",
                      manager->model, ret);
        AIRY_FREE(cache_text);
        AIRY_FREE(cache_key);
        return ret;
    }

    note_truncation(resp, hit.model, hit.max_tokens);
    update_cost_tracking(svc, hit.model, resp);

    /* 写入键改用实际模型：同一逻辑请求换端点后答案不可互相冒充（H8）。 */
    char *store_key = make_cache_key(hit.model, cache_text);
    if (store_key) {
        cache_store(svc, store_key, cache_text, hit.model, resp, manager->cacheable);
        AIRY_FREE(store_key);
    } else {
        SVC_LOG_WARN("C-L02: SVC: cache store skipped (key alloc failed, model=%s)", hit.model);
    }

    *out_response = resp;
    AIRY_FREE(cache_text);
    AIRY_FREE(cache_key);
    return AIRY_OK;
}

int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;

    cfg_probe(svc);

    uint32_t gen = 0;
    int erc = req_enter(svc, &gen, "llm_service_complete");
    if (erc != 0)
        return erc;

    int rc = complete_impl(svc, manager, out_response);
    if (req_leave(svc, gen, out_response) && rc == AIRY_OK)
        rc = AIRY_ERR_BUSY;
    return rc;
}

static int stream_impl(llm_service_t *svc, const llm_request_config_t *manager,
                       llm_stream_callback_t callback, void *callback_data,
                       llm_response_t **out_response)
{
    int grc = req_guard(svc, manager, callback != NULL, "STREAM",
                        "llm_service_complete_stream");
    if (grc != AIRY_OK)
        return grc;

    llm_route_request_t req;
    build_route_request(svc, manager, true, &req);

    const provider_t *prov = pick_provider(svc, manager, &req, "llm_service_complete_stream", true);
    if (!prov)
        return AIRY_ERR_LLM_INVALID_MODEL;

    log_routing_prelude(manager, "stream_user_specified");

    stream_ctx_t sc = {.cb = callback, .ud = callback_data, .sent = 0};
    svc_hit_t hit;
    __builtin_memset(&hit, 0, sizeof(hit));
    llm_response_t *resp = NULL;

    int ret = svc_chain_run(svc, manager, &req, prov, stream_attempt, &sc, stream_sent, &resp, &hit);
    if (ret != AIRY_OK)
        return ret;

    if (resp) {
        note_truncation(resp, hit.model, hit.max_tokens);
        update_cost_tracking(svc, hit.model, resp);
    }

    if (out_response)
        *out_response = resp;
    else
        llm_response_free(resp);
    return AIRY_OK;
}

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;

    cfg_probe(svc);

    uint32_t gen = 0;
    int erc = req_enter(svc, &gen, "llm_service_complete_stream");
    if (erc != 0)
        return erc;

    int rc = stream_impl(svc, manager, callback, callback_data, out_response);
    if (req_leave(svc, gen, out_response) && rc == AIRY_OK) {
        /* 已下发的分片无法追回，但聚合结果同样不得回流。 */
        rc = AIRY_ERR_BUSY;
    }
    return rc;
}

/* OpenAI 兼容：仅 Bearer 鉴权，Content-Type 由 core 统一附加。 */
static const provider_header_spec_t EMBEDDINGS_HEADERS = {.auth = PROVIDER_AUTH_BEARER};

static int emb_impl(llm_service_t *svc, const char *model, const char *request_body, char **out_json)
{
    if (!svc || !request_body || !request_body[0] || !out_json)
        return AIRY_ERR_INVALID_PARAM;
    *out_json = NULL;

    const char *m = (model && model[0]) ? model : svc->default_model;
    const provider_t *prov = svc_find_prov(svc, m ? m : "");
    if (!prov || !prov->ctx) {
        SVC_LOG_ERROR("embeddings: no provider for model=%s", m ? m : "(default)");
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    provider_refresh_api_key(base);

    if (!base->api_base[0]) {
        SVC_LOG_ERROR("embeddings: empty api_base for provider=%s", prov->name ? prov->name : "?");
        return AIRY_ERR_INVALID_PARAM;
    }

    /* B16-S6：出网收敛到 providers/core 唯一入口，URL 拼装、头链、超时与
     * 重试全归 core，service 层不出现 curl_*。 */
    provider_request_t req = {
        .base = base,
        .path = "/embeddings",
        .headers = &EMBEDDINGS_HEADERS,
        .body = request_body,
    };

    provider_http_resp_t *resp = NULL;
    long http_code = 0;
    int rc = provider_http_exec(&req, &resp, &http_code);

    if (rc != AIRY_OK || !resp) {
        provider_http_resp_free(resp);
        SVC_LOG_ERROR("embeddings: HTTP request failed api_base=%s rc=%d", base->api_base, rc);
        return AIRY_ERR_IO;
    }

    if (http_code >= 400) {
        SVC_LOG_WARN("embeddings: upstream HTTP %ld api_base=%s", http_code, base->api_base);
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    *out_json = AIRY_STRDUP(resp->data ? resp->data : "{}");
    provider_http_resp_free(resp);
    if (!*out_json)
        return AIRY_ERR_OUT_OF_MEMORY;
    return AIRY_SUCCESS;
}

int llm_service_embeddings(llm_service_t *svc, const char *model, const char *request_body,
                           char **out_json)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;

    cfg_probe(svc);

    uint32_t gen = 0;
    int erc = req_enter(svc, &gen, "llm_service_embeddings");
    if (erc != 0)
        return erc;

    int rc = emb_impl(svc, model, request_body, out_json);
    if (!svc_swap_leave(&svc->swap, gen)) {
        if (out_json && *out_json) {
            AIRY_FREE(*out_json);
            *out_json = NULL;
        }
        SVC_LOG_WARN("C-L02: SVC: embeddings dropped — model generation advanced mid-request");
        if (rc == AIRY_SUCCESS)
            rc = AIRY_ERR_BUSY;
    }
    return rc;
}
