// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file service.c
 * @brief LLM 服务核心逻辑实现
 *
 * 改进说明：
 * 1. 修复 stpcpy 不可移植问题
 * 2. 统一错误码为 AIRY_ERR_*
 * 3. 完善 YAML 解析逻辑
 * 4. 线程安全
 */

#include "daemon_defaults.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "response.h"
#include "router/llm_router.h"
#include "service.h"
#include "svc_logger.h"

#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_YAML
#include <yaml.h>
#endif

#include "llm_service_internal.h"

/**
 * @brief Safe string concatenation
 * @param dest      Destination string
 * @param dest_size Destination buffer size
 * @param src       Source string
 * @return End position after writing
 */
static char *safe_strcat(char *dest, size_t dest_size, const char *src) __attribute__((unused));
static char *safe_strcat(char *dest, size_t dest_size, const char *src)
{
    size_t dest_len = strlen(dest);
    size_t remaining = dest_size - dest_len - 1;
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < remaining) ? src_len : remaining;

    if (copy_len > 0) {
        __builtin_memcpy(dest + dest_len, src, copy_len);
        dest[dest_len + copy_len] = '\0';
    }

    return dest + dest_len + copy_len;
}

llm_service_t *llm_service_create(const char *config_path)
{
    llm_service_t *svc = (llm_service_t *)AIRY_CALLOC(1, sizeof(llm_service_t));
    if (!svc) {
        SVC_LOG_ERROR(
            "C-L02: SVC: CREATE-FAIL allocate service context, STACK: llm_service_create");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

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

    /* P0.18.3 fix: load model config (providers/models list) from the manager
     * to fill base_cfg. Previously llm_service_create never called
     * svc_load_model_config, the registry stayed empty, the llm_router had
     * zero endpoints, and every model request failed routing (INVALID_MODEL). */
    provider_config_t *model_providers = NULL;
    size_t model_provider_count = 0;
    if (config_path) {
        int cfg_ret = svc_load_model_config(config_path, &model_providers, &model_provider_count);
        if (cfg_ret == 0 && model_providers && model_provider_count > 0) {
            /* A2-1: user override file $AIRY_CONFIG_DIR/model.yaml (user
             * wins; same-name providers replaced). Users can add/remove
             * providers/models or change defaults without touching the repo
             * SSoT. */
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

    /* A2-1: global.default_model/default_provider take effect (main config +
     * user override, user wins). Previously the global section of model.yaml
     * was consumed by no code; the default model relied on gateway hardcoding. */
    char global_model[128] = {0};
    char global_provider[64] = {0};
    if (config_path)
        svc_model_defaults_from_yaml(config_path, global_model, sizeof(global_model),
                                     global_provider, sizeof(global_provider));
    {
        char user_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir) {
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path)) {
                FILE *uf = fopen(user_path, "rb");
                if (uf) {
                    fclose(uf);
                    char um[128] = {0};
                    char up[64] = {0};
                    svc_model_defaults_from_yaml(user_path, um, sizeof(um), up, sizeof(up));
                    if (um[0])
                        AIRY_STRNCPY_TERM(global_model, um, sizeof(global_model));
                    if (up[0])
                        AIRY_STRNCPY_TERM(global_provider, up, sizeof(global_provider));
                }
            }
        }
    }
    if (global_model[0]) {
        AIRY_STRNCPY_TERM(svc->default_model, global_model, sizeof(svc->default_model));
        SVC_LOG_INFO("C-L02: SVC: default_model=%s (from global config)", svc->default_model);
    } else if (config_path) {

        svc_model_llm_config_t llm_cfg;
        __builtin_memset(&llm_cfg, 0, sizeof(llm_cfg));
        if (svc_model_defaults_llm_from_yaml(config_path, &llm_cfg) == 0 && llm_cfg.model[0]) {
            AIRY_STRNCPY_TERM(svc->default_model, llm_cfg.model, sizeof(svc->default_model));
            if (!global_provider[0] && llm_cfg.api_format[0]) {
                const char *adapter =
                    (strcasecmp(llm_cfg.api_format, "anthropic") == 0) ? "anthropic" : "openai";
                AIRY_STRNCPY_TERM(svc->default_provider, adapter, sizeof(svc->default_provider));
            }
            SVC_LOG_INFO("C-L02: SVC: default_model=%s (from llm section)", svc->default_model);
        }
    }
    if (global_provider[0])
        AIRY_STRNCPY_TERM(svc->default_provider, global_provider, sizeof(svc->default_provider));

    /* Parse pricing rules (uses cJSON; JSON config only).
     * model.yaml is YAML; feeding YAML content straight to cJSON must fail
     * parsing, wrongly reporting a "Failed to parse pricing rules" WARN at
     * every startup while pricing rules never load. Currently neither
     * model.yaml nor model.json has a pricing section; YAML config degrades
     * gracefully to no rules, same semantics as the JSON-sourced config
     * (missing pricing -> no cost rules, DEBUG only). */
    if (config_path && ends_with(config_path, ".json")) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long yaml_len = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (yaml_len <= 0) {
                fclose(f);
            } else {
                char *yaml_content = (char *)AIRY_MALLOC((size_t)yaml_len + 1);
                if (yaml_content) {
                    size_t read_len = fread(yaml_content, 1, (size_t)yaml_len, f);
                    if (read_len != (size_t)yaml_len) {
                        AIRY_FREE(yaml_content);
                        yaml_content = NULL;
                    }
                    if (yaml_content) {
                        yaml_content[read_len] = '\0';

                        /* P0.18.2: CJSON_PARSE_GUARD replaces cJSON_Parse +
                         * if (root) + manual cJSON_Delete, using do { ... }
                         * while (0) + break to preserve the original
                         * if (root) ... else ... block semantics */
                        do {
                            CJSON_PARSE_GUARD(root, yaml_content, {
                                SVC_LOG_WARN("Failed to parse pricing rules from manager");
                                break;
                            });
                            int rule_count = 0;
                            pricing_rule_t *rules = load_pricing_rules(root, &rule_count);
                            if (rules && rule_count > 0) {
                                svc->rules = rules;
                                svc->rule_count = rule_count;
                                SVC_LOG_INFO("Loaded %d pricing rules", rule_count);
                            } else if (rules) {
                                AIRY_FREE(rules);
                            }

                        } while (0);
                    }
                    AIRY_FREE(yaml_content);
                } else {
                    SVC_LOG_ERROR("Failed to allocate memory for manager content");
                }
                fclose(f);
            }
        }
    } else if (config_path) {
        SVC_LOG_DEBUG("pricing rules: YAML config has no cJSON pricing section, "
                      "degraded to no cost rules");
    }

    svc->registry = provider_registry_create(&base_cfg);
    if (!svc->registry) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL registry, STACK: llm_service_create");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* P0.18.3: the registry deep-copied the provider config; release the
     * temporarily loaded model_providers (including the merged array after
     * user override) */
    if (model_providers) {
        free_provider_configs(model_providers, model_provider_count);
        model_providers = NULL;
    }

    svc->cache = llm_cache_create(base_cfg.llm_cache_capacity, base_cfg.llm_cache_ttl_sec);
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
        llm_cache_destroy(svc->cache);
        provider_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    svc->token_counter = token_counter_create(base_cfg.token_encoding);
    if (!svc->token_counter) {
        SVC_LOG_ERROR("C-L02: SVC: CREATE-FAIL token_counter, STACK: llm_service_create");
        cost_tracker_destroy(svc->cost);
        llm_cache_destroy(svc->cache);
        provider_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* P3.16 (ACC-DT17): init llm_router and register the registry's
     * provider/model as routing endpoints. The router is a global singleton
     * (design in llm_router.c); init is idempotent. Failure is non-fatal: the
     * complete path falls back to find_provider for backward compatibility. */
    if (llm_router_init(NULL) != 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: llm_router_init failed — routing disabled, falling back to find_provider");
    } else {
        register_router_endpoints(svc);
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
        llm_cache_destroy(svc->cache);
        svc->cache = NULL;
    }

    if (svc->cost) {
        cost_tracker_destroy(svc->cost);
        svc->cost = NULL;
    }

    if (svc->token_counter) {
        token_counter_destroy(svc->token_counter);
        svc->token_counter = NULL;
    }

    if (svc->rules) {
        free_pricing_rules((pricing_rule_t *)svc->rules, (int)svc->rule_count);
        svc->rules = NULL;
        svc->rule_count = 0;
    }

    /* P3.16 (ACC-DT17): destroy the global router singleton.
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
            AIRY_FREE((void *)resp->choices[i].tool_call_id);
            AIRY_FREE((void *)resp->choices[i].tool_calls_json);
        }
        AIRY_FREE(resp->choices);
    }
    AIRY_FREE(resp);
}
