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
#include "rpc/rpc_internal.h"

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

/* 创建期装配状态：配置面一次读入，再分阶段落到 svc。 */
typedef struct {
    service_config_t base;
    provider_config_t *provs;
    size_t prov_n;
    char model[128];
    char pname[64];
    svc_model_llm_config_t llm;
    const char *src;
} llm_boot_t;

/* 用户覆盖文件 $AIRY_CONFIG_DIR/model.yaml：可容纳且已存在时返回 1。 */
static int llm_user_path(char *buf, size_t n)
{
    const char *dir = airy_config_dir();
    if (!dir)
        return 0;
    int len = snprintf(buf, n, "%s/model.yaml", dir);
    if (len <= 0 || len >= (int)n)
        return 0;
    FILE *f = fopen(buf, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* 主配置 providers 与用户覆盖合并（同名用户胜出）；失败留空注册表。 */
static void llm_prov_load(const char *cfg, llm_boot_t *b)
{
    int rc = svc_load_model_config(cfg, &b->provs, &b->prov_n);
    if (rc != 0 || !b->provs || b->prov_n == 0) {
        free_provider_configs(b->provs, b->prov_n);
        b->provs = NULL;
        b->prov_n = 0;
        SVC_LOG_WARN("C-L02: SVC: model config load failed (ret=%d), "
                     "registry will be empty",
                     rc);
        return;
    }
    char up[1024];
    provider_config_t *uprov = NULL;
    size_t un = 0;
    if (llm_user_path(up, sizeof(up)) && svc_load_model_config(up, &uprov, &un) == 0 && uprov &&
        un > 0) {
        provider_config_t *merged = NULL;
        size_t mn = 0;
        merge_provider_configs(b->provs, b->prov_n, uprov, un, &merged, &mn);
        if (merged && mn > 0) {
            free_provider_configs(b->provs, b->prov_n);
            b->provs = merged;
            b->prov_n = mn;
            SVC_LOG_INFO("C-L02: SVC: merged %zu user provider(s) from %s "
                         "(user overrides same-name)",
                         un, up);
        }
    }
    free_provider_configs(uprov, un);
    SVC_LOG_INFO("C-L02: SVC: loaded %zu provider(s) from manager config", b->prov_n);
}

/* llm 段优先、缺省回退 models 表首条（v2 表格格式）；返回来源描述。 */
static const char *llm_pick_cfg(const char *cfg, svc_model_llm_config_t *out)
{
    if (svc_model_defaults_llm_from_yaml(cfg, out) == 0 && out->model[0])
        return "llm section";
    __builtin_memset(out, 0, sizeof(*out));
    if (svc_model_defaults_models0_from_yaml(cfg, out) == 0 && out->model[0])
        return "models table";
    return NULL;
}

/* 默认模型/提供方/输出上限的读取与用户覆盖合并（只读配置，不落 svc）。 */
static void llm_def_load(llm_service_t *svc, const char *cfg, llm_boot_t *b)
{
    char up[1024];
    if (cfg) {
        svc_model_defaults_from_yaml(cfg, b->model, sizeof(b->model), b->pname, sizeof(b->pname));
        /* 默认模型的来源与输出上限相互独立：上限仍须从 llm 段（或 models
         * 表首条）读出。 */
        b->src = llm_pick_cfg(cfg, &b->llm);
    }
    if (!llm_user_path(up, sizeof(up)))
        return;
    /* 用户只覆盖自己显式写下的 routing 键（不重置主配置）。 */
    (void)svc_route_load(up, &svc->route);
    char um[128] = {0};
    char upv[64] = {0};
    svc_model_defaults_from_yaml(up, um, sizeof(um), upv, sizeof(upv));
    if (um[0])
        AIRY_STRNCPY_TERM(b->model, um, sizeof(b->model));
    if (upv[0])
        AIRY_STRNCPY_TERM(b->pname, upv, sizeof(b->pname));

    /* 用户覆盖同样 user wins：调低 max_output 须立即生效。 */
    svc_model_llm_config_t uc;
    __builtin_memset(&uc, 0, sizeof(uc));
    (void)llm_pick_cfg(up, &uc);
    if (uc.model[0] && uc.max_output_tokens > 0)
        b->llm.max_output_tokens = uc.max_output_tokens;
}

/* 默认模型/提供方/上限落 svc：default_model 段优先，否则取 llm 段来源。 */
static void llm_def_apply(llm_service_t *svc, const llm_boot_t *b)
{
    if (b->model[0]) {
        AIRY_STRNCPY_TERM(svc->default_model, b->model, sizeof(svc->default_model));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from default_model config)",
                     svc->default_model);
    } else if (b->src) {
        AIRY_STRNCPY_TERM(svc->default_model, b->llm.model, sizeof(svc->default_model));
        /* 适配器按线格式命名（chat | messages | local），配置面的 api_format
         * 即适配器查找键，无需再做厂商名到适配器的映射。 */
        if (!b->pname[0] && b->llm.api_format[0])
            AIRY_STRNCPY_TERM(svc->default_provider, b->llm.api_format,
                              sizeof(svc->default_provider));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from %s)", svc->default_model, b->src);
    }
    if (b->pname[0])
        AIRY_STRNCPY_TERM(svc->default_provider, b->pname, sizeof(svc->default_provider));
    if (b->llm.max_output_tokens > 0) {
        svc->default_max_output_tokens = b->llm.max_output_tokens;
        SVC_LOG_INFO("C-L02: SVC: default_max_output_tokens=%d (from model config)",
                     svc->default_max_output_tokens);
    }
}

/* 价格唯一来源是 model.yaml 的 models[].input/output_cost_per_1k；未配置
 * 价格的模型由 cost_tracker 落默认价。 */
static void llm_price_load(llm_service_t *svc, const char *cfg)
{
    pricing_rule_t *rules = NULL;
    int n = 0;
    if (load_pricing_rules_from_yaml(cfg, &rules, &n) == 0 && rules && n > 0) {
        svc->rules = rules;
        svc->rule_count = n;
        SVC_LOG_INFO("Loaded %d pricing rules from YAML model config", n);
    } else if (rules) {
        AIRY_FREE(rules);
    }
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
    if (llm_user_path(up, sizeof(up)) && (!cfg || strcmp(cfg, up) != 0))
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

    /* 0.1.19 §3.8.4 A4：routing 段；缺失即保持 off，现网零变化。 */
    if (config_path)
        (void)svc_route_load(config_path, &svc->route);

    if (airy_mtx_init(&svc->lock) != 0) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL init lock, STACK: llm_service_create");
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    llm_boot_t b;
    __builtin_memset(&b, 0, sizeof(b));
    b.base.llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    b.base.llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    b.base.max_retries = AIRY_DEFAULT_MAX_RETRIES;
    b.base.timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;

    if (config_path) {
        llm_prov_load(config_path, &b);
        b.base.providers = b.provs;
        b.base.provider_count = b.prov_n;
        llm_price_load(svc, config_path);
    }
    /* 用户覆盖独立于主配置路径：cfg 缺席时 $AIRY_CONFIG_DIR 仍生效。 */
    llm_def_load(svc, config_path, &b);
    llm_def_apply(svc, &b);

    if (svc->route.mode == SVC_CHAIN_AUTO)
        SVC_LOG_INFO("C-L02: SVC: routing=auto depth=%d on=0x%x budget_ms=%d", svc->route.depth,
                     svc->route.on_mask, svc->route.budget_ms);

    if (llm_parts_new(svc, &b.base) != 0) {
        free_provider_configs(b.provs, b.prov_n);
        llm_parts_drop(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    /* 注册表已深拷贝 provider 配置，临时数组（含用户覆盖合并结果）释放。 */
    free_provider_configs(b.provs, b.prov_n);

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
