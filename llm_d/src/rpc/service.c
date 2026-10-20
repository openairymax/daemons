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
    static int built = 0;
    if (!built) {
        const char *data_dir = airy_data_dir();
        if (!data_dir) {
            return NULL;
        }
        char dir[1024];
        int dlen = snprintf(dir, sizeof(dir), "%s/agentrt", data_dir);
        if (dlen < 0 || dlen >= (int)sizeof(dir))
            return NULL;
        if (airy_mkdir_p(dir) != 0)
            return NULL;
        int plen = snprintf(path, sizeof(path), "%s/llm_usage.json", dir);
        if (plen < 0 || plen >= (int)sizeof(path))
            return NULL;
        built = 1;
    }
    return path;
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

    service_config_t base_cfg;
    __builtin_memset(&base_cfg, 0, sizeof(base_cfg));
    base_cfg.llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    base_cfg.llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    base_cfg.max_retries = AIRY_DEFAULT_MAX_RETRIES;
    base_cfg.timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;

    /* 加载 model.yaml 的 providers/models 清单填充注册表；空注册表会使
     * 所有请求路由失败（INVALID_MODEL）。 */
    provider_config_t *model_providers = NULL;
    size_t model_provider_count = 0;
    if (config_path) {
        int cfg_ret = svc_load_model_config(config_path, &model_providers, &model_provider_count);
        if (cfg_ret == 0 && model_providers && model_provider_count > 0) {
            /* A2-1：用户覆盖 $AIRY_CONFIG_DIR/model.yaml 优先，同名替换。 */
            char user_path[1024];
            int has_user_cfg = 0;
            const char *cfg_dir = airy_config_dir();
            if (cfg_dir) {
                int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
                if (plen > 0 && plen < (int)sizeof(user_path)) {
                    FILE *uf = fopen(user_path, "rb");
                    if (uf) {
                        fclose(uf);
                        has_user_cfg = 1;
                    }
                }
            }
            if (has_user_cfg) {
                provider_config_t *user_providers = NULL;
                size_t user_provider_count = 0;
                int u_ret = svc_load_model_config(user_path, &user_providers, &user_provider_count);
                if (u_ret == 0 && user_providers && user_provider_count > 0) {
                    provider_config_t *merged = NULL;
                    size_t merged_count = 0;
                    merge_provider_configs(model_providers, model_provider_count, user_providers,
                                           user_provider_count, &merged, &merged_count);
                    if (merged && merged_count > 0) {
                        free_provider_configs(model_providers, model_provider_count);
                        free_provider_configs(user_providers, user_provider_count);
                        model_providers = merged;
                        model_provider_count = merged_count;
                        SVC_LOG_INFO("C-L02: SVC: merged %zu user provider(s) from %s "
                                     "(user overrides same-name)",
                                     user_provider_count, user_path);
                    } else {
                        free_provider_configs(user_providers, user_provider_count);
                    }
                } else if (user_providers) {
                    free_provider_configs(user_providers, user_provider_count);
                }
            }
            base_cfg.providers = model_providers;
            base_cfg.provider_count = model_provider_count;
            SVC_LOG_INFO("C-L02: SVC: loaded %zu provider(s) from manager config",
                         model_provider_count);
        } else {
            SVC_LOG_WARN("C-L02: SVC: model config load failed (ret=%d), "
                         "registry will be empty",
                         cfg_ret);
        }
    }

    /* A2-1：global 段默认模型/提供方生效（主配置 + 用户覆盖，用户优先）。 */
    char global_model[128] = {0};
    char global_provider[64] = {0};
    if (config_path)
        svc_model_defaults_from_yaml(config_path, global_model, sizeof(global_model),
                                     global_provider, sizeof(global_provider));

    /* 默认模型的来源与输出上限相互独立：上限仍须从 llm 段（或 models 表
     * 首条）读出。 */
    svc_model_llm_config_t llm_cfg;
    const char *gen_src = NULL;
    __builtin_memset(&llm_cfg, 0, sizeof(llm_cfg));
    if (config_path) {
        if (svc_model_defaults_llm_from_yaml(config_path, &llm_cfg) == 0 && llm_cfg.model[0]) {
            gen_src = "llm section";
        } else {
            /* v2 表格格式（2026-08-26）：llm 段缺省时回退 models 表首个条目 */
            __builtin_memset(&llm_cfg, 0, sizeof(llm_cfg));
            if (svc_model_defaults_models0_from_yaml(config_path, &llm_cfg) == 0 &&
                llm_cfg.model[0])
                gen_src = "models table";
        }
    }

    {
        char user_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir) {
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path)) {
                FILE *uf = fopen(user_path, "rb");
                if (uf) {
                    fclose(uf);
                    /* 用户只覆盖自己显式写下的 routing 键（不重置主配置）。 */
                    (void)svc_route_load(user_path, &svc->route);
                    char um[128] = {0};
                    char up[64] = {0};
                    svc_model_defaults_from_yaml(user_path, um, sizeof(um), up, sizeof(up));
                    if (um[0])
                        AIRY_STRNCPY_TERM(global_model, um, sizeof(global_model));
                    if (up[0])
                        AIRY_STRNCPY_TERM(global_provider, up, sizeof(global_provider));

                    /* 用户覆盖同样 user wins：调低 max_output 须立即生效。 */
                    svc_model_llm_config_t ucfg;
                    __builtin_memset(&ucfg, 0, sizeof(ucfg));
                    int u_ok = (svc_model_defaults_llm_from_yaml(user_path, &ucfg) == 0 &&
                                ucfg.model[0]);
                    if (!u_ok) {
                        __builtin_memset(&ucfg, 0, sizeof(ucfg));
                        u_ok = (svc_model_defaults_models0_from_yaml(user_path, &ucfg) == 0 &&
                                ucfg.model[0]);
                    }
                    if (u_ok && ucfg.max_output_tokens > 0)
                        llm_cfg.max_output_tokens = ucfg.max_output_tokens;
                }
            }
        }
    }

    if (svc->route.mode == SVC_CHAIN_AUTO)
        SVC_LOG_INFO("C-L02: SVC: routing=auto depth=%d on=0x%x budget_ms=%d", svc->route.depth,
                     svc->route.on_mask, svc->route.budget_ms);

    if (global_model[0]) {
        AIRY_STRNCPY_TERM(svc->default_model, global_model, sizeof(svc->default_model));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from default_model config)",
                     svc->default_model);
    } else if (gen_src) {
        AIRY_STRNCPY_TERM(svc->default_model, llm_cfg.model, sizeof(svc->default_model));
        if (!global_provider[0] && llm_cfg.api_format[0]) {
            const char *adapter =
                (strcasecmp(llm_cfg.api_format, "anthropic") == 0) ? "anthropic" : "openai";
            AIRY_STRNCPY_TERM(svc->default_provider, adapter, sizeof(svc->default_provider));
        }
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from %s)", svc->default_model, gen_src);
    }
    if (global_provider[0])
        AIRY_STRNCPY_TERM(svc->default_provider, global_provider, sizeof(svc->default_provider));

    if (llm_cfg.max_output_tokens > 0) {
        svc->default_max_output_tokens = llm_cfg.max_output_tokens;
        SVC_LOG_INFO("C-L02: SVC: default_max_output_tokens=%d (from model config)",
                     svc->default_max_output_tokens);
    }

    /* 定价规则只从 YAML 声明面提取（models[].input/output_cost_per_1k）；
     * JSON 面的 pricing 段随双轨收敛摘除（model.json 本就无该段）。 */
    if (config_path) {
        /* 价格唯一来源是 model.yaml 的 models[].input/output_cost_per_1k；
         * 未配置价格的模型由 cost_tracker 落默认价。 */
        pricing_rule_t *yaml_rules = NULL;
        int yaml_rule_count = 0;
        if (load_pricing_rules_from_yaml(config_path, &yaml_rules, &yaml_rule_count) == 0 &&
            yaml_rules && yaml_rule_count > 0) {
            svc->rules = yaml_rules;
            svc->rule_count = yaml_rule_count;
            SVC_LOG_INFO("Loaded %d pricing rules from YAML model config", yaml_rule_count);
        } else if (yaml_rules) {
            AIRY_FREE(yaml_rules);
        }
    }

    svc->registry = provider_registry_create(&base_cfg);
    if (!svc->registry) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL registry, STACK: llm_service_create");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* The registry deep-copied the provider config; release the
     * temporarily loaded model_providers (including the merged array after
     * user override) */
    if (model_providers) {
        free_provider_configs(model_providers, model_provider_count);
        model_providers = NULL;
    }

    svc->cache = cache_create_string_cache(base_cfg.llm_cache_capacity, base_cfg.llm_cache_ttl_sec);
    if (!svc->cache) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cache, STACK: llm_service_create");
        provider_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    svc->cost = cost_tracker_create((const pricing_rule_t *)svc->rules, (int)svc->rule_count);
    if (!svc->cost) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL cost_tracker, STACK: llm_service_create");
        cache_destroy(svc->cache);
        provider_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* 加载历史计费/用量累计（幂等，无文件则跳过）。 */
    const char *usage_path = llm_usage_state_path();
    if (usage_path) {
        if (cost_tracker_load(svc->cost, usage_path) == 0)
            SVC_LOG_INFO("C-L02: SVC: cost state loaded from %s", usage_path);
        else
            SVC_LOG_WARN("C-L02: SVC: cost state load failed (ignored) path=%s", usage_path);
    }

    /* Init llm_router and register the registry's
     * provider/model as routing endpoints. The router is a global singleton
     * (design in llm_router.c); init is idempotent. Failure is non-fatal: the
     * complete path falls back to find_provider for backward compatibility. */
    if (llm_router_init() != 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: llm_router_init failed — routing disabled, falling back to find_provider");
    } else {
        register_router_endpoints(svc);
    }

    /* 0.1.19 §3.5 缺陷5：对启动时真正消费的两份 model.yaml 照快照。
     * manager 配置与用户覆盖 (A2-1) 只要路径不同即各照一份，用户直接
     * 编辑其中任一文件都能在请求侧被检出；缺席者 valid=0 静默停用。 */
    if (config_path)
        (void)svc_mw_init(&svc->mw, config_path, "llm_d");
    {
        char user_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir) {
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path) &&
                (!config_path || strcmp(config_path, user_path) != 0))
                (void)svc_mw_init(&svc->mw_usr, user_path, "llm_d");
        }
    }

    SVC_LOG_INFO("C-L02: SVC: CREATE-OK pricing_rules=%d llm_cache_capacity=%zu cache_ttl=%u",
                 (int)svc->rule_count, base_cfg.llm_cache_capacity, base_cfg.llm_cache_ttl_sec);
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
