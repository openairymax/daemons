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

#include <stdlib.h>
#include <string.h>
#include "error.h"

struct provider_registry {
    provider_t *providers;
    airy_mtx_t lock;
};

/* 模型名表深拷贝：计数 + 逐项复制，任一环失败即全量回滚。
 * 返回 NULL 同时表示"无模型表"与"复制失败"，两者对调用方同义
 * （无模型即不可路由）。 */
static char **models_dup(const provider_config_t *pcfg, size_t *cnt_out)
{
    size_t cnt = 0;

    if (!pcfg->models)
        return NULL;
    while (pcfg->models[cnt])
        cnt++;

    char **models = AIRY_CALLOC(cnt + 1, sizeof(*models));

    if (!models)
        return NULL;
    for (size_t j = 0; j < cnt; ++j) {
        models[j] = AIRY_STRDUP(pcfg->models[j]);
        if (!models[j]) {
            SVC_LOG_ERROR("Failed to duplicate model name: out of memory");
            for (size_t k = 0; k < j; ++k)
                AIRY_FREE(models[k]);
            AIRY_FREE(models);
            return NULL;
        }
    }
    *cnt_out = cnt;
    return models;
}

/* 与 models 同长同序的整型副表；尾部多分配一个零元素，与 models 的
 * NUL 尾槽对齐，便于按序索引。 */
static int *ints_dup(const int *src, size_t n)
{
    int *dst = AIRY_CALLOC(n + 1, sizeof(*dst));

    if (!dst)
        return NULL;
    for (size_t j = 0; j < n; ++j)
        dst[j] = src[j];
    return dst;
}

static void models_free(char **models)
{
    if (!models)
        return;
    for (char **m = models; *m; ++m)
        AIRY_FREE(*m);
    AIRY_FREE(models);
}

/* 组装单个 provider 槽位。返回 0 表示槽位就绪，-1 表示整条跳过（槽位
 * 保持 calloc 原样，由调用方决定是否推进写指针）。 */
static int provider_build(const provider_config_t *pcfg, provider_t *slot)
{
    /* provider_adapter_lookup 契约永不为 NULL：未知名回落行协议适配 */
    const provider_adapter_t *adapter = provider_adapter_lookup(pcfg->api_format);
    provider_ctx_t *ctx = adapter->init(pcfg->name, pcfg->api_key, pcfg->api_base,
                                       pcfg->organization, pcfg->timeout_sec,
                                       pcfg->max_retries);

    if (!ctx) {
        SVC_LOG_ERROR("Failed to init provider: %s", pcfg->name);
        return -1;
    }

    if (pcfg->extra_headers && pcfg->extra_headers[0])
        AIRY_STRNCPY_TERM(provider_base_ctx(ctx)->extra_headers, pcfg->extra_headers,
                          sizeof(provider_base_ctx(ctx)->extra_headers));
    provider_base_ctx(ctx)->caps = pcfg->caps;

    size_t cnt = 0;
    char **models = models_dup(pcfg, &cnt);
    int *caps = (models && pcfg->model_max_output && cnt > 0)
                    ? ints_dup(pcfg->model_max_output, cnt)
                    : NULL;
    int *cncs = (models && pcfg->model_max_concurrent && cnt > 0)
                    ? ints_dup(pcfg->model_max_concurrent, cnt)
                    : NULL;
    char *name = AIRY_STRDUP(pcfg->name);

    if (!name) {
        SVC_LOG_ERROR("Failed to duplicate provider name: out of memory");
        models_free(models);
        AIRY_FREE(caps);
        AIRY_FREE(cncs);
        adapter->destroy(ctx);
        return -1;
    }

    slot->name = name;
    slot->adapter = adapter;
    slot->ctx = ctx;
    slot->models = models;
    slot->model_max_output = caps;
    slot->model_max_concurrent = cncs;
    return 0;
}

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
        airy_mtx_destroy(&reg->lock);
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
        if (provider_build(pcfg, &reg->providers[valid]) == 0)
            valid++;
    }

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
            AIRY_FREE(p->model_max_concurrent);
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

size_t provider_registry_count(provider_registry_t *reg)
{
    if (!reg)
        return 0;

    size_t n = 0;
    airy_mtx_lock(&reg->lock);
    if (reg->providers) {
        for (provider_t *p = reg->providers; p->name; ++p)
            n++;
    }
    airy_mtx_unlock(&reg->lock);
    return n;
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

int provider_reg_walk(provider_registry_t *reg,
                      int (*cb)(const provider_t *prov, void *user_data), void *user_data)
{
    if (!reg || !cb) {
        return 0;
    }

    airy_mtx_lock(&reg->lock);
    if (!reg->providers) {
        airy_mtx_unlock(&reg->lock);
        return 0;
    }

    /* reg->lock is held during the callback, so the callback must copy what it
     * needs and perform no I/O nor any registry call that re-acquires this
     * lock (e.g. provider_registry_find). A non-zero callback return
     * short-circuits the iteration. */
    int short_circuit = 0;
    for (provider_t *p = reg->providers; p->name; ++p) {
        if (cb(p, user_data) != 0) {
            short_circuit = 1;
            break;
        }
    }

    airy_mtx_unlock(&reg->lock);
    return short_circuit;
}

typedef struct reg_enum_s {
    int (*cb)(const char *provider_name, const char *model_name, void *user_data);
    void *user_data;
} reg_enum_t;

static int reg_enum_step(const provider_t *p, void *user_data)
{
    reg_enum_t *a = (reg_enum_t *)user_data;
    if (!p->models)
        return 0;
    for (char **m = p->models; *m; ++m) {
        if (a->cb(p->name, *m, a->user_data) != 0)
            return 1;
    }
    return 0;
}

int provider_registry_enumerate(provider_registry_t *reg,
                                int (*cb)(const char *provider_name, const char *model_name,
                                          void *user_data),
                                void *user_data)
{
    reg_enum_t a = {cb, user_data};
    return provider_reg_walk(reg, reg_enum_step, &a);
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
 * 耗时上界由 rpc/methods.c 的 LLM_RETRY_FAST_FAIL_MS 共同保证
 * （3 × 5s + 70s = 85s < 90s）。模型行可用 timeout_sec 显式覆盖；覆盖后
 * 须自行保证仍小于网关背压。 */
#define PROVIDER_DEFAULT_TIMEOUT_SEC 70.0

/* Bounded copy: keeps dst untouched when src does not fit, so an over-long
 * upstream value degrades to the previous (default) content. */
static void copy_fit(char *dst, size_t cap, const char *src)
{
    size_t len = src ? strlen(src) : cap;

    if (len < cap)
        __builtin_memcpy(dst, src, len + 1);
}

void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name, const char *api_key,
                        const char *api_base, const char *organization, double timeout_sec,
                        int max_retries, const char *default_base)
{
    if (!base_ctx)
        return;

    __builtin_memset(base_ctx, 0, sizeof(provider_base_ctx_t));

    copy_fit(base_ctx->name, sizeof(base_ctx->name), provider_name);

    /* Record the api_key_env name (for secrets.env hot-reload on request).
     * Prefer extracting from the "env:NAME" prefix; otherwise derive the
     * conventional env var name from the declared provider name (brand-neutral
     * rule in core/secrets.c, no vendor table). */
    base_ctx->api_key_env[0] = '\0';
    if (api_key && strncmp(api_key, "env:", 4) == 0) {
        copy_fit(base_ctx->api_key_env, sizeof(base_ctx->api_key_env), api_key + 4);
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

    copy_fit(base_ctx->api_key, sizeof(base_ctx->api_key), resolved_key);

    if (api_base || default_base) {
        copy_fit(base_ctx->api_base, sizeof(base_ctx->api_base),
                 api_base ? api_base : default_base);
    }

    copy_fit(base_ctx->organization, sizeof(base_ctx->organization), organization);

    base_ctx->timeout_sec = timeout_sec > 0 ? timeout_sec : PROVIDER_DEFAULT_TIMEOUT_SEC;
    base_ctx->max_retries = max_retries > 0 ? max_retries : 3;
    base_ctx->retry_class = PROVIDER_RETRY_DEFAULT;

    SVC_LOG_INFO("C-L02: PROVIDER: BASE-INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 base_ctx->api_base[0] ? base_ctx->api_base : "(none)", base_ctx->timeout_sec,
                 base_ctx->max_retries, base_ctx->api_key[0] ? 1 : 0);
}

provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx)
{
    /* 约定：所有 provider 的 ctx 首字段均为 provider_base_ctx_t base */
    return (provider_base_ctx_t *)ctx;
}
