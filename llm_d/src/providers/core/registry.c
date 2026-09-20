// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file registry.c
 * @brief Provider registry implementation.
 */

#include "daemon_platform_ext.h"
#include "registry.h"
#include "secrets.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

struct provider_registry {
    provider_t *providers;
    airy_mtx_t lock;
};

provider_registry_t *provider_registry_create(const service_config_t *cfg)
{
    provider_registry_t *reg = AIRY_CALLOC(1, sizeof(provider_registry_t));
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    airy_mtx_init(&reg->lock);

    size_t count = cfg->provider_count;
    if (count == 0 || !cfg->providers)
        return reg;

    reg->providers = AIRY_CALLOC(count + 1, sizeof(provider_t));
    if (!reg->providers) {
        AIRY_FREE(reg);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* Compacting writer: the array is NUL-terminated by its trailing calloc'd
     * slot, so every skipped entry (nameless / init failure / OOM) must NOT
     * leave a hole. A hole would terminate the `p->name` walks in find /
     * enumerate / destroy, silently hiding the providers behind it and
     * leaking their ctx and model strings. */
    size_t valid = 0;
    for (size_t i = 0; i < count; ++i) {
        const provider_config_t *pcfg = &cfg->providers[i];
        if (!pcfg->name || !pcfg->name[0]) {
            SVC_LOG_WARN("Provider entry #%zu has no name, skipping", i);
            continue;
        }
        /* provider_adapter_lookup 契约永不为 NULL：未知名回落 openai 兼容 */
        const provider_adapter_t *adapter = provider_adapter_lookup(pcfg->name);

        provider_ctx_t *ctx = adapter->init(pcfg->name, pcfg->api_key, pcfg->api_base,
                                        pcfg->organization, pcfg->timeout_sec, pcfg->max_retries);
        if (!ctx) {
            SVC_LOG_ERROR("Failed to init provider: %s", pcfg->name);
            continue;
        }

        char **models = NULL;
        int *caps = NULL;
        size_t model_cnt = 0;
        if (pcfg->models) {
            while (pcfg->models[model_cnt])
                model_cnt++;
            models = AIRY_CALLOC(model_cnt + 1, sizeof(*models));
            if (models) {
                for (size_t j = 0; j < model_cnt; ++j) {
                    models[j] = AIRY_STRDUP(pcfg->models[j]);
                    if (!models[j]) {
                        SVC_LOG_ERROR("Failed to duplicate model name: out of memory");
                        for (size_t k = 0; k < j; ++k)
                            AIRY_FREE(models[k]);
                        AIRY_FREE(models);
                        models = NULL;
                        break;
                    }
                }
            }
        }
        if (models && pcfg->model_max_output && model_cnt > 0) {
            caps = AIRY_CALLOC(model_cnt + 1, sizeof(*caps));
            if (caps) {
                for (size_t j = 0; j < model_cnt; ++j)
                    caps[j] = pcfg->model_max_output[j];
            }
        }

        reg->providers[valid].name = AIRY_STRDUP(pcfg->name);
        if (!reg->providers[valid].name) {
            SVC_LOG_ERROR("Failed to duplicate provider name: out of memory");
            if (models) {
                for (size_t j = 0; models[j]; ++j)
                    AIRY_FREE(models[j]);
                AIRY_FREE(models);
            }
            AIRY_FREE(caps);
            adapter->destroy(ctx);
            continue;
        }
        reg->providers[valid].adapter = adapter;
        reg->providers[valid].ctx = ctx;
        reg->providers[valid].models = models;
        reg->providers[valid].model_max_output = caps;
        valid++;
    }

    return reg;
}

provider_registry_t *provider_registry_create_from_config(const service_config_t *cfg,
                                                          const char *config_path)
{
    provider_registry_t *reg = provider_registry_create(cfg);
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (!config_path)
        return reg;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("Cannot open provider config '%s'", config_path);
        return reg;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        return reg;
    }

    char *content = (char *)AIRY_MALLOC((size_t)len + 1);
    if (!content) {
        fclose(f);
        return reg;
    }

    size_t read_len = fread(content, 1, (size_t)len, f);
    content[read_len] = '\0';
    fclose(f);

    CJSON_PARSE_GUARD(root, content, {
        AIRY_FREE(content);
        SVC_LOG_WARN("Failed to parse provider config '%s'", config_path);
        return reg;
    });
    AIRY_FREE(content);

    cJSON *providers_arr = cJSON_GetObjectItem(root, "providers");
    if (!providers_arr || !cJSON_IsArray(providers_arr)) {

        SVC_LOG_WARN("No 'providers' array in '%s'", config_path);
        return reg;
    }

    int n = cJSON_GetArraySize(providers_arr);
    if (n <= 0) {

        return reg;
    }

    airy_mtx_lock(&reg->lock);

    size_t old_count = 0;
    if (reg->providers) {
        while (reg->providers[old_count].name)
            old_count++;
    }

    size_t new_count = (size_t)n;
    provider_t *new_provs =
        (provider_t *)AIRY_CALLOC(old_count + new_count + 1, sizeof(provider_t));
    if (!new_provs) {
        airy_mtx_unlock(&reg->lock);

        return reg;
    }

    if (reg->providers) {
        __builtin_memcpy(new_provs, reg->providers, old_count * sizeof(provider_t));
        AIRY_FREE(reg->providers);
    }
    reg->providers = new_provs;

    size_t valid_idx = old_count;
    for (int i = 0; i < n; ++i) {
        cJSON *pitem = cJSON_GetArrayItem(providers_arr, i);
        cJSON *pname = cJSON_GetObjectItem(pitem, "name");
        cJSON *pkey_env = cJSON_GetObjectItem(pitem, "api_key_env");
        cJSON *pkey = cJSON_GetObjectItem(pitem, "api_key");
        cJSON *pbase = cJSON_GetObjectItem(pitem, "base_url");
        cJSON *ptimeout = cJSON_GetObjectItem(pitem, "timeout_sec");
        cJSON *pretries = cJSON_GetObjectItem(pitem, "max_retries");
        cJSON *pmodels = cJSON_GetObjectItem(pitem, "models");

        if (!cJSON_IsString(pname))
            continue;

        const char *name_str = pname->valuestring;
        /* provider_adapter_lookup 契约永不为 NULL：未知名回落 openai 兼容 */
        const provider_adapter_t *adapter = provider_adapter_lookup(name_str);

        char api_key_buf[512] = {0};
        if (cJSON_IsString(pkey_env) && pkey_env->valuestring[0]) {
            /* Keep the "env:NAME" form: provider_base_init extracts the env
             * name for secrets.env hot-reload on request (filling the key
             * after startup needs no restart) */
            size_t env_len = strlen(pkey_env->valuestring);
            if (4 + env_len < sizeof(api_key_buf)) {
                __builtin_memcpy(api_key_buf, "env:", 4);
                __builtin_memcpy(api_key_buf + 4, pkey_env->valuestring, env_len + 1);
            }
        } else if (cJSON_IsString(pkey) && pkey->valuestring[0]) {
            AIRY_STRNCPY_TERM(api_key_buf, pkey->valuestring, sizeof(api_key_buf));
            (api_key_buf)[sizeof(api_key_buf) - 1] = '\0';
        }

        const char *base_str = cJSON_IsString(pbase) ? pbase->valuestring : NULL;
        double timeout = cJSON_IsNumber(ptimeout) ? ptimeout->valuedouble : 30.0;
        int retries = cJSON_IsNumber(pretries) ? pretries->valueint : 3;

        provider_ctx_t *ctx = adapter->init(name_str, api_key_buf[0] ? api_key_buf : NULL, base_str,
                                        NULL, timeout, retries);
        if (!ctx) {
            SVC_LOG_ERROR("Failed to init provider '%s' from config", name_str);
            continue;
        }

        char **models = NULL;
        if (cJSON_IsArray(pmodels)) {
            int mcount = cJSON_GetArraySize(pmodels);
            models = (char **)AIRY_CALLOC((size_t)mcount + 1, sizeof(char *));
            if (models) {
                for (int j = 0; j < mcount; ++j) {
                    cJSON *mitem = cJSON_GetArrayItem(pmodels, j);
                    if (cJSON_IsString(mitem)) {
                        models[j] = AIRY_STRDUP(mitem->valuestring);
                        if (!models[j]) {
                            SVC_LOG_ERROR("Failed to duplicate model name: out of memory");
                            for (int k = 0; k < j; ++k)
                                AIRY_FREE(models[k]);
                            AIRY_FREE(models);
                            models = NULL;
                            break;
                        }
                    }
                }
                if (models)
                    models[mcount] = NULL;
            }
        }

        reg->providers[valid_idx].name = AIRY_STRDUP(name_str);
        if (!reg->providers[valid_idx].name) {
            SVC_LOG_ERROR("Failed to duplicate provider name: out of memory");
            if (models) {
                for (int j = 0; models[j]; ++j)
                    AIRY_FREE(models[j]);
                AIRY_FREE(models);
            }
            adapter->destroy(ctx);
            continue;
        }
        reg->providers[valid_idx].adapter = adapter;
        reg->providers[valid_idx].ctx = ctx;
        reg->providers[valid_idx].models = models;
        valid_idx++;
    }

    airy_mtx_unlock(&reg->lock);

    SVC_LOG_INFO("Loaded %zu providers from config '%s'", valid_idx - old_count, config_path);
    return reg;
}

void provider_registry_destroy(provider_registry_t *reg)
{
    if (!reg)
        return;
    airy_mtx_lock(&reg->lock);
    if (reg->providers) {
        for (provider_t *p = reg->providers; p->name; ++p) {
            p->adapter->destroy(p->ctx);
            AIRY_FREE((void *)p->name);
            if (p->models) {
                for (char **m = p->models; *m; ++m)
                    AIRY_FREE(*m);
                AIRY_FREE(p->models);
            }
            AIRY_FREE(p->model_max_output);
        }
        AIRY_FREE(reg->providers);
        reg->providers = NULL;
    }
    airy_mtx_unlock(&reg->lock);
    airy_mtx_destroy(&reg->lock);
    AIRY_FREE(reg);
}

const provider_t *provider_registry_find(provider_registry_t *reg, const char *model)
{
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    airy_mtx_lock(&reg->lock);
    if (!reg->providers) {
        airy_mtx_unlock(&reg->lock);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    for (provider_t *p = reg->providers; p->name; ++p) {
        if (!p->models)
            continue;
        for (char **m = p->models; *m; ++m) {
            if (strcmp(*m, model) == 0) {
                airy_mtx_unlock(&reg->lock);
                return p;
            }
        }
    }
    airy_mtx_unlock(&reg->lock);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

int provider_registry_model_max_output(const provider_t *prov, const char *model)
{
    if (!prov || !model || !prov->models || !prov->model_max_output)
        return 0;
    for (size_t i = 0; prov->models[i]; ++i) {
        if (strcmp(prov->models[i], model) == 0)
            return prov->model_max_output[i];
    }
    return 0;
}

int provider_registry_enumerate(provider_registry_t *reg,
                                int (*cb)(const char *provider_name, const char *model_name,
                                          void *user_data),
                                void *user_data)
{
    if (!reg || !cb) {
        return 0;
    }

    airy_mtx_lock(&reg->lock);
    if (!reg->providers) {
        airy_mtx_unlock(&reg->lock);
        return 0;
    }

    /* P3.16 (ACC-DT17): iterate all (provider, model) pairs and callback.
     * Note: reg->lock is held during the callback, so the callback must not
     * call registry functions that acquire the same lock (e.g.
     * provider_registry_find). Router endpoint registration uses its own
     * router mutex, no deadlock risk. A non-zero callback return short-circuits
     * the iteration. */
    int short_circuit = 0;
    for (provider_t *p = reg->providers; p->name; ++p) {
        if (!p->name || !p->models)
            continue;
        for (char **m = p->models; *m; ++m) {
            int rc = cb(p->name, *m, user_data);
            if (rc != 0) {
                short_circuit = 1;
                break;
            }
        }
        if (short_circuit)
            break;
    }

    airy_mtx_unlock(&reg->lock);
    return short_circuit;
}

/* 单次 provider HTTP 调用默认超时（秒）。
 *
 * 不变量（R-5）：llm_d 处理一次 complete 的总耗时必须严格小于网关的转发
 * 超时背压（gateway_biz_internal.h 的 GW_LLM_DEFAULT_TIMEOUT_MS=90s、
 * GW_THINK_TIMEOUT_MS=120s）。否则网关会先于 llm_d 放弃请求，用户只能
 * 看到笼统的 "invalid response"，而 provider 侧已经定位好的精确诊断
 * （鉴权失败 / 限流 / 连接失败）永远回传不到用户。
 *
 * 该上界默认值单独保证"只发一次请求"的场景（70s < 90s）；多次重试的总
 * 耗时上界由 llm_daemon_methods.c 的 LLM_RETRY_FAST_FAIL_MS 共同保证
 * （3 × 5s + 70s = 85s < 90s）。模型行可用 timeout_sec 显式覆盖；覆盖后
 * 须自行保证仍小于网关背压。 */
#define PROVIDER_DEFAULT_TIMEOUT_SEC 70.0

void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name, const char *api_key,
                        const char *api_base, const char *organization, double timeout_sec,
                        int max_retries, const char *default_base)
{
    if (!base_ctx)
        return;

    __builtin_memset(base_ctx, 0, sizeof(provider_base_ctx_t));

    /* Record the api_key_env name (for secrets.env hot-reload on request).
     * Prefer extracting from the "env:NAME" prefix; otherwise derive the
     * conventional env var name from the declared provider name (brand-neutral
     * rule in core/secrets.c, no vendor table). */
    base_ctx->api_key_env[0] = '\0';
    if (api_key && strncmp(api_key, "env:", 4) == 0) {
        const char *env_name = api_key + 4;
        size_t env_len = strlen(env_name);

        if (env_len > 0 && env_len < sizeof(base_ctx->api_key_env)) {
            __builtin_memcpy(base_ctx->api_key_env, env_name, env_len + 1);
        }
    } else {
        char env_name[128];
        if (sec_env_name_for(provider_name, env_name, sizeof(env_name))) {
            AIRY_STRNCPY_TERM(base_ctx->api_key_env, env_name, sizeof(base_ctx->api_key_env));
        }
    }

    const char *resolved_key = sec_resolve_key(api_key);
    if ((!resolved_key || resolved_key[0] == '\0') && base_ctx->api_key_env[0]) {
        resolved_key = getenv(base_ctx->api_key_env);
    }

    if (resolved_key) {
        size_t key_len = strlen(resolved_key);
        if (key_len < sizeof(base_ctx->api_key)) {
            __builtin_memcpy(base_ctx->api_key, resolved_key, key_len + 1);
        }
    }

    if (api_base) {
        size_t base_len = strlen(api_base);
        if (base_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, api_base, base_len + 1);
        }
    } else if (default_base) {
        size_t default_len = strlen(default_base);
        if (default_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, default_base, default_len + 1);
        }
    }

    if (organization) {
        size_t org_len = strlen(organization);
        if (org_len < sizeof(base_ctx->organization)) {
            __builtin_memcpy(base_ctx->organization, organization, org_len + 1);
        }
    }

    base_ctx->timeout_sec = timeout_sec > 0 ? timeout_sec : PROVIDER_DEFAULT_TIMEOUT_SEC;
    base_ctx->max_retries = max_retries > 0 ? max_retries : 3;

    SVC_LOG_INFO("C-L02: PROVIDER: BASE-INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 base_ctx->api_base[0] ? base_ctx->api_base : "(none)", base_ctx->timeout_sec,
                 base_ctx->max_retries, base_ctx->api_key[0] ? 1 : 0);
}

provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx)
{
    /* 约定：所有 provider 的 ctx 首字段均为 provider_base_ctx_t base */
    return (provider_base_ctx_t *)ctx;
}
