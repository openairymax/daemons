// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file service.c
 * @brief LLM service core logic implementation.
 *
 * Improvements:
 * 1. Fixed the non-portable stpcpy issue
 * 2. Unified error codes to AIRY_ERR_*
 * 3. Completed the YAML parsing logic
 * 4. Thread safety
 */

#include "daemon_defaults.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "response.h"
#include "router/core/llm_router.h"
#include "service.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_internal.h"
#include "providers/core/prov_stat.h"
#include "rpc/rpc_internal.h"
#include "rpc/service_boot.h"

/* 装配头：仅用于可见 llm_svc_reg 原型（防签名漂移）。 */
#include "llm_d_internal.h"

/* 跨域桥（0.1.19 D5）：向装配域/RPC 方法域暴露 provider registry 句柄，
 * 使 struct llm_service 保持私有不被穿透。见 llm_d_internal.h 契约注释。 */
provider_registry_t *llm_svc_reg(const llm_service_t *svc)
{
    if (!svc)
        return NULL;
    const struct llm_service *s = (const struct llm_service *)svc;
    return s->registry;
}

/* 计费/用量持久化路径 $AIRY_DATA_DIR/agentrt/llm_usage.json，重启不清零。 */
const char *llm_usage_state_path(void)
{
    static char path[1024];
    static int built;
    if (built)
        return path;
    const char *data_dir = airy_data_dir();
    char dir[1024];
    int dlen = data_dir ? snprintf(dir, sizeof(dir), "%s/agentrt", data_dir) : -1;
    if (dlen <= 0 || dlen >= (int)sizeof(dir) || airy_mkdir_p(dir) != 0)
        return NULL;
    int plen = snprintf(path, sizeof(path), "%s/llm_usage.json", dir);
    if (plen <= 0 || plen >= (int)sizeof(path))
        return NULL;
    built = 1;
    return path;
}

/* 组件装配：注册表 → 缓存 → 计费；任一失败由调用方统一回滚。 */
static int llm_parts_new(llm_service_t *svc, const service_config_t *base)
{
    svc->registry = provider_registry_create(base);
    if (!svc->registry) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL registry, STACK: llm_service_create");
        return -1;
    }
    svc->cache = cache_create_string_cache(base->llm_cache_capacity, base->llm_cache_ttl_sec);
    if (!svc->cache) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cache, STACK: llm_service_create");
        return -1;
    }
    svc->cost = cost_tracker_create((const pricing_rule_t *)svc->rules, (int)svc->rule_count);
    if (!svc->cost) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cost_tracker, STACK: llm_service_create");
        return -1;
    }
    return 0;
}

/* 创建失败回滚：各 destroy 对 NULL 安全，按装配反序释放。 */
static void llm_parts_drop(llm_service_t *svc)
{
    cost_tracker_destroy(svc->cost);
    cache_destroy(svc->cache);
    provider_registry_destroy(svc->registry);
    free_pricing_rules((pricing_rule_t *)svc->rules, (int)svc->rule_count);
    svc_swap_done(&svc->swap);
    airy_mtx_destroy(&svc->reload_lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

/* 路由是进程级单例（llm_router.c）且 init 幂等；失败非致命，complete
 * 路径回退 find_provider。 */
static void llm_route_init(llm_service_t *svc)
{
    if (llm_router_init() != 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: llm_router_init failed — routing disabled, falling back to find_provider");
        return;
    }
    register_router_endpoints(svc);
}

/* 0.1.19 §3.5 缺陷5：对启动时真正消费的两份 model.yaml 照快照。
 * manager 配置与用户覆盖 (A2-1) 只要路径不同即各照一份，用户直接编辑
 * 其中任一文件都能在请求侧被检出；缺席者 valid=0 静默停用。 */
static void llm_watch_cfg(llm_service_t *svc, const char *cfg)
{
    char up[1024];
    if (cfg)
        (void)svc_mw_init(&svc->mw, cfg, "llm_d");
    if (llm_boot_usr_path(up, sizeof(up)) && (!cfg || strcmp(cfg, up) != 0))
        (void)svc_mw_init(&svc->mw_usr, up, "llm_d");
}

llm_service_t *llm_service_create(const char *config_path)
{
    llm_service_t *svc = (llm_service_t *)AIRY_CALLOC(1, sizeof(llm_service_t));
    if (!svc) {
        SVC_LOG_ERROR(
            "C-L02: SVC: CREATE-FAIL allocate service context, STACK: llm_service_create");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* CALLOC 全零会让 depth=0，必须显式落缺省（0.1.19 §3.8.4 A3）。 */
    svc_chain_def(&svc->route);

    if (airy_mtx_init(&svc->lock) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init lock, STACK: llm_service_create");
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (airy_mtx_init(&svc->reload_lock) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init reload_lock, STACK: llm_service_create");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (svc_swap_init(&svc->swap, SVC_SWAP_DRAIN_MS) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init swap, STACK: llm_service_create");
        airy_mtx_destroy(&svc->reload_lock);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (config_path)
        AIRY_STRNCPY_TERM(svc->cfg_path, config_path, sizeof(svc->cfg_path));

    /* 0.1.19 §3.8.4 A4：routing 段随 boot 一次读入；缺失即保持 off。 */
    llm_boot_t b;
    llm_boot_fill(&b, config_path);
    llm_boot_apply(svc, &b);

    if (svc->route.mode == SVC_CHAIN_AUTO)
        SVC_LOG_INFO("C-L02: SVC: routing=auto depth=%d on=0x%x budget_ms=%d", svc->route.depth,
                     svc->route.on_mask, svc->route.budget_ms);

    if (llm_parts_new(svc, &b.base) != 0) {
        llm_boot_drop(&b);
        llm_parts_drop(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    /* 注册表已深拷贝 provider 配置，暂存态（含用户覆盖合并结果）释放。 */
    llm_boot_drop(&b);

    /* 加载历史计费/用量累计（幂等，无文件则跳过）。 */
    const char *usage_path = llm_usage_state_path();
    if (usage_path) {
        if (cost_tracker_load(svc->cost, usage_path) == 0)
            SVC_LOG_INFO("C-L02: SVC: cost state loaded from %s", usage_path);
        else
            SVC_LOG_WARN("C-L02: SVC: cost state load failed (ignored) path=%s", usage_path);
    }

    llm_route_init(svc);
    llm_watch_cfg(svc, config_path);

    SVC_LOG_INFO("C-L02: SVC: CREATE-OK pricing_rules=%d llm_cache_capacity=%zu cache_ttl=%u",
                 (int)svc->rule_count, b.base.llm_cache_capacity, b.base.llm_cache_ttl_sec);
    return svc;
}

void llm_service_destroy(llm_service_t *svc)
{
    if (!svc)
        return;

    SVC_LOG_INFO("C-L02: SVC: DESTROY");

    /* 退休表：热重载排空超时后旧装载仍未归零，只能留到这里统一释放。 */
    for (int i = 0; i < svc->retired_n; i++) {
        if (svc->retired[i].registry)
            provider_registry_destroy(svc->retired[i].registry);
        if (svc->retired[i].rules)
            free_pricing_rules(svc->retired[i].rules, svc->retired[i].rule_count);
    }
    svc->retired_n = 0;

    if (svc->registry) {
        provider_registry_destroy(svc->registry);
        svc->registry = NULL;
    }

    if (svc->cache) {
        cache_destroy(svc->cache);
        svc->cache = NULL;
    }

    if (svc->cost) {
        /* 退出前保存计费/用量累计，重启后不清零。 */
        const char *usage_path = llm_usage_state_path();
        if (usage_path && cost_tracker_save(svc->cost, usage_path) == 0)
            SVC_LOG_INFO("C-L02: SVC: cost state saved to %s", usage_path);
        cost_tracker_destroy(svc->cost);
        svc->cost = NULL;
    }

    if (svc->rules) {
        free_pricing_rules((pricing_rule_t *)svc->rules, (int)svc->rule_count);
        svc->rules = NULL;
        svc->rule_count = 0;
    }

    /* Destroy the global router singleton.
     * Note: the router is a process-level global singleton; this assumes
     * llm_service is a single instance in the daemon (paired with
     * llm_router_init on the create path). llm_router_init is idempotent, so
     * it can be re-initialized when the service is recreated. Each test is a
     * separate executable process, so there is no cross-instance global-state
     * pollution. */
    llm_router_destroy();

    svc_swap_done(&svc->swap);
    airy_mtx_destroy(&svc->reload_lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

void llm_response_free(llm_response_t *resp)
{
    if (!resp)
        return;
    AIRY_FREE(resp->id);
    AIRY_FREE(resp->model);
    AIRY_FREE(resp->finish_reason);
    if (resp->choices) {
        for (size_t i = 0; i < resp->choice_count; i++) {
            AIRY_FREE((void *)resp->choices[i].role);
            AIRY_FREE((void *)resp->choices[i].content);
            AIRY_FREE((void *)resp->choices[i].reasoning_content);
            AIRY_FREE((void *)resp->choices[i].tool_call_id);
            AIRY_FREE((void *)resp->choices[i].tool_calls_json);
        }
        AIRY_FREE(resp->choices);
    }
    AIRY_FREE(resp);
}

int llm_service_stats(llm_service_t *svc, char **out_json)
{
    if (!svc || !out_json) {
        SVC_LOG_ERROR("llm_service_stats: NULL parameter (svc=%p, out_json=%p)", (const void *)svc,
                      (const void *)out_json);
        return AIRY_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_CreateObject failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *cost_json = cost_tracker_export(svc->cost);
    if (cost_json) {
        cJSON_AddItemToObject(root, "cost", cost_json);
    }

    cJSON_AddNumberToObject(root, "llm_cache_size", cache_get_size(svc->cache));
    cJSON_AddNumberToObject(root, "llm_cache_capacity", cache_get_capacity(svc->cache));

    cache_stats_t cache_stats;
    cache_get_stats(svc->cache, &cache_stats);
    cJSON_AddNumberToObject(root, "llm_cache_hits", (double)cache_stats.hits);
    cJSON_AddNumberToObject(root, "llm_cache_misses", (double)cache_stats.misses);
    cJSON_AddNumberToObject(root, "llm_cache_evictions", (double)cache_stats.evictions);
    cJSON_AddNumberToObject(root, "llm_cache_hit_rate", cache_stats.hit_rate);

    llm_router_stats_t rs;
    if (llm_router_get_stats(&rs) == 0) {
        static const char *const k_route[] = {"complexity", "cost",   "latency",
                                              "fallback",   "round_robin"};
        cJSON *router = cJSON_CreateObject();
        if (router) {
            cJSON_AddNumberToObject(router, "total_requests", (double)rs.total_requests);
            for (size_t i = 0; i < sizeof(k_route) / sizeof(k_route[0]); i++) {
                char key[32];
                snprintf(key, sizeof(key), "routed_%s", k_route[i]);
                cJSON_AddNumberToObject(router, key, (double)rs.routed_count[i]);
            }
            cJSON_AddNumberToObject(router, "fallback_count", (double)rs.fallback_count);
            cJSON_AddNumberToObject(router, "error_count", (double)rs.error_count);
            cJSON_AddNumberToObject(router, "total_cost", rs.total_cost);
            cJSON_AddNumberToObject(router, "total_tokens", (double)rs.total_tokens);
            cJSON_AddItemToObject(root, "router", router);
        }
        cJSON *meas = cJSON_CreateObject();
        if (meas) {
            cJSON_AddNumberToObject(meas, "lat_samples", (double)rs.lat_samples);
            cJSON_AddNumberToObject(meas, "lat_total_ms", (double)rs.lat_total_ms);
            cJSON_AddNumberToObject(meas, "avg_latency_ms",
                                    rs.lat_samples ? (double)rs.lat_total_ms /
                                                         (double)rs.lat_samples
                                                   : 0.0);
            cJSON_AddNumberToObject(meas, "prompt_tokens", (double)rs.prompt_tokens_sum);
            cJSON_AddNumberToObject(meas, "cached_tokens", (double)rs.cached_tokens_sum);
            /* G47：分列与样本数并出，禁止以 aggregate 冒充性能；判定层为端点自报。 */
            cJSON_AddNumberToObject(meas, "meas_samples", (double)rs.meas_samples);
            cJSON_AddNumberToObject(meas, "prefill_tokens", (double)rs.prefill_tokens);
            cJSON_AddNumberToObject(meas, "decode_tokens", (double)rs.decode_tokens);
            cJSON_AddNumberToObject(meas, "kv_cache_tokens", (double)rs.kv_cache_tokens);
            cJSON_AddNumberToObject(meas, "prefill_ms", rs.prefill_ms);
            cJSON_AddNumberToObject(meas, "decode_ms", rs.decode_ms);
            cJSON_AddItemToObject(root, "measurement", meas);
        }
    }

    /* D2-9：超时三拆的传输侧观测。三类成因处置不同——连接未建立可无条件
     * 重试、上游停摆仅在幂等时可重试、整轮预算耗尽不可重试；混为一个
     * "超时"计数会掩盖根因。 */
    cJSON *transport = cJSON_CreateObject();
    if (transport) {
        cJSON_AddNumberToObject(transport, "connect_timeout",
                                (double)prov_stat_get(PROV_TMO_CONNECT));
        cJSON_AddNumberToObject(transport, "stream_stall",
                                (double)prov_stat_get(PROV_TMO_STALL));
        cJSON_AddNumberToObject(transport, "total_timeout",
                                (double)prov_stat_get(PROV_TMO_TOTAL));
        cJSON_AddItemToObject(root, "transport", transport);
    }

    /* D2-10：热切换观测。gen 与 mismatch 并出——只见 gen 增长而看不到被
     * 丢弃的旧世代回包，会把强制排空误读为"切换无损"。 */
    cJSON *swap = cJSON_CreateObject();
    if (swap) {
        uint32_t gen = 0;
        uint32_t inflight = 0;
        uint32_t swapped = 0;
        uint64_t mismatch = 0;
        uint64_t drain_tmo = 0;
        svc_swap_stat(&svc->swap, &gen, &inflight, &swapped, &mismatch, &drain_tmo);
        cJSON_AddNumberToObject(swap, "gen", (double)gen);
        cJSON_AddNumberToObject(swap, "inflight", (double)inflight);
        cJSON_AddNumberToObject(swap, "swapped", (double)swapped);
        cJSON_AddNumberToObject(swap, "gen_mismatch", (double)mismatch);
        cJSON_AddNumberToObject(swap, "drain_timeout", (double)drain_tmo);
        cJSON_AddNumberToObject(swap, "reload_ok", (double)svc->reload_ok);
        cJSON_AddNumberToObject(swap, "reload_fail", (double)svc->reload_fail);
        cJSON_AddItemToObject(root, "swap", swap);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_PrintUnformatted failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    *out_json = json;
    return AIRY_OK;
}

typedef struct {
    cJSON *models_arr;
    const char *default_model;
} list_models_ctx_t;

static int list_models_cb(const char *provider_name, const char *model_name, void *user_data)
{
    list_models_ctx_t *ctx = (list_models_ctx_t *)user_data;
    if (!ctx || !ctx->models_arr || !provider_name || !model_name)
        return 0;

    cJSON *item = cJSON_CreateObject();
    if (!item)
        return 0;
    cJSON_AddStringToObject(item, "name", model_name);
    cJSON_AddStringToObject(item, "provider", provider_name);
    cJSON_AddBoolToObject(item, "default",
                          ctx->default_model && strcmp(ctx->default_model, model_name) == 0);
    cJSON_AddItemToArray(ctx->models_arr, item);
    return 0;
}

char *llm_service_list_models(llm_service_t *svc)
{
    if (!svc)
        return NULL;

    airy_mtx_lock(&svc->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON *models_arr = cJSON_CreateArray();
    if (!models_arr) {
        cJSON_Delete(root);
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON_AddItemToObject(root, "models", models_arr);

    list_models_ctx_t ctx = {
        .models_arr = models_arr,
        .default_model = svc->default_model[0] ? svc->default_model : NULL,
    };
    provider_registry_enumerate(svc->registry, list_models_cb, &ctx);

    cJSON_AddStringToObject(root, "default_model", svc->default_model[0] ? svc->default_model : "");
    if (svc->default_provider[0])
        cJSON_AddStringToObject(root, "default_provider", svc->default_provider);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    airy_mtx_unlock(&svc->lock);
    return json;
}

const char *llm_service_default_model(const llm_service_t *svc)
{
    if (!svc)
        return NULL;
    return svc->default_model[0] ? svc->default_model : NULL;
}
