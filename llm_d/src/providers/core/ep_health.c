// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ep_health.c
 * @brief LLM 端点健康探测机制件（llm_d providers/core，0.1.19 D5）。
 *
 * "能力存在 ≠ 能力可用"：provider 注册表只回答"配了哪些端点"，不能回答
 * "此刻端点是否可达、冷加载代价多大"。本件为该问题提供唯一机制实现——
 * 按创建期快照的 api_base 发起幂等只读探测，并把请求侧观测到的冷加载
 * 特征并回同一视图，供 llm_d health_check 与 airymaxrt status 呈现。
 *
 * 机制与策略分离：探测周期、探测范围（filter）、探测路径由策略层
 * （svc.c 经 manifest 与配置）注入；本件只负责"探测一次、到账一次"。
 *
 * 并发：sweep 运行在 daemon 线程池 worker、cold 运行在 rpc 请求线程、
 * json 运行在状态读取侧，三者以 lock 串行化对视图的读写；出网 I/O 一律
 * 在锁外完成后回填，绝不持锁出网。
 *
 * 生命期：create 期一次性取 registry 快照，此后不再引用 registry，故
 * handle 与 registry 解耦——registry 先销毁也不产生悬垂指针。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "svc_logger.h"

#include "ep_health.h"
#include "provider_transport.h"

#include <cjson/cJSON.h>
#include <string.h>

#define EP_HEALTH_MAX_ENDPOINTS 16
#define EP_HEALTH_PROBE_TIMEOUT_SEC 8.0
#define EP_HEALTH_COLD_MS 10000.0
#define EP_HEALTH_DEFAULT_PATH "/models"

typedef struct {
    char provider[64];
    char base[512];
    int reachable; /* -1 未知 / 0 不可达 / 1 可达 */
    long http_code;
    double latency_ms;
    int cold;
    char cold_model[64];
    double cold_ms;
} ep_rec_t;

/* 探测快照：只携带出网所需字段，逐端点拷出后在锁外使用。 */
typedef struct {
    char provider[64];
    char base[512];
} ep_probe_t;

struct ep_health {
    char filter[64];
    char probe_path[64];
    airy_mtx_t lock;
    size_t count;
    ep_rec_t recs[EP_HEALTH_MAX_ENDPOINTS];
};

static int ep_filter_hit(const char *filter, const char *name)
{
    return filter[0] == '\0' || strcmp(filter, name) == 0;
}

static int ep_snap_cb(const provider_t *prov, void *ud)
{
    ep_health_t *h = (ep_health_t *)ud;
    if (!prov || !prov->name || !ep_filter_hit(h->filter, prov->name))
        return 0;

    if (h->count >= EP_HEALTH_MAX_ENDPOINTS) {
        SVC_LOG_WARN("C-L02: EP-HEALTH: endpoint view full at %d, remaining endpoints ignored",
                     EP_HEALTH_MAX_ENDPOINTS);
        return 1;
    }

    ep_rec_t *rec = &h->recs[h->count];
    AIRY_STRNCPY_TERM(rec->provider, prov->name, sizeof(rec->provider));

    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    if (base && base->api_base[0])
        AIRY_STRNCPY_TERM(rec->base, base->api_base, sizeof(rec->base));

    rec->reachable = -1;
    h->count++;
    return 0;
}

ep_health_t *ep_health_create(provider_registry_t *reg, const char *filter, const char *probe_path)
{
    if (!reg)
        return NULL;

    ep_health_t *h = (ep_health_t *)AIRY_CALLOC(1, sizeof(ep_health_t));
    if (!h)
        return NULL;

    if (filter && filter[0])
        AIRY_STRNCPY_TERM(h->filter, filter, sizeof(h->filter));
    AIRY_STRNCPY_TERM(h->probe_path,
                      (probe_path && probe_path[0]) ? probe_path : EP_HEALTH_DEFAULT_PATH,
                      sizeof(h->probe_path));

    airy_mtx_init(&h->lock);
    provider_reg_walk(reg, ep_snap_cb, h);

    SVC_LOG_INFO("C-L02: EP-HEALTH: view created filter=%s path=%s endpoints=%zu",
                 h->filter[0] ? h->filter : "*", h->probe_path, h->count);
    return h;
}

void ep_health_destroy(ep_health_t *h)
{
    if (!h)
        return;
    airy_mtx_destroy(&h->lock);
    AIRY_FREE(h);
}

static void ep_probe_one(const char *base, const char *path, int *out_ok, long *out_code,
                         double *out_ms)
{
    *out_ok = 0;
    *out_code = 0;
    *out_ms = 0.0;

    char url[1024];
    if (provider_url_join(base, path, url, sizeof(url)) != AIRY_OK)
        return;

    uint64_t t0 = airy_time_ms();
    provider_http_resp_t *resp = NULL;
    long code = 0;
    int rc = provider_http_get(url, NULL, EP_HEALTH_PROBE_TIMEOUT_SEC, &resp, &code);
    double ms = (double)(airy_time_ms() - t0);
    provider_http_resp_free(resp);

    *out_code = code;
    *out_ms = ms;
    *out_ok = (rc == AIRY_OK && code >= 200 && code < 400);
}

void ep_health_sweep(ep_health_t *h)
{
    if (!h)
        return;

    /* 锁内取探测快照，锁外出网：任何网络等待都不得阻塞 cold/json 的读侧。 */
    ep_probe_t snap[EP_HEALTH_MAX_ENDPOINTS];
    size_t n;
    airy_mtx_lock(&h->lock);
    n = h->count;
    for (size_t i = 0; i < n; i++) {
        AIRY_STRNCPY_TERM(snap[i].provider, h->recs[i].provider, sizeof(snap[i].provider));
        AIRY_STRNCPY_TERM(snap[i].base, h->recs[i].base, sizeof(snap[i].base));
    }
    airy_mtx_unlock(&h->lock);

    for (size_t i = 0; i < n; i++) {
        int ok = 0;
        long code = 0;
        double ms = 0.0;
        ep_probe_one(snap[i].base, h->probe_path, &ok, &code, &ms);

        airy_mtx_lock(&h->lock);
        for (size_t j = 0; j < h->count; j++) {
            if (strcmp(h->recs[j].provider, snap[i].provider) == 0) {
                /* 只刷新可达性三态；cold 字段由请求侧独立维护，不互相覆盖。 */
                h->recs[j].reachable = ok;
                h->recs[j].http_code = code;
                h->recs[j].latency_ms = ms;
                break;
            }
        }
        airy_mtx_unlock(&h->lock);

        if (ok)
            SVC_LOG_DEBUG("C-L02: EP-HEALTH: probe ok provider=%s http=%ld latency_ms=%.1f",
                          snap[i].provider, code, ms);
        else
            SVC_LOG_WARN("C-L02: EP-HEALTH: probe FAIL provider=%s base=%s http=%ld "
                         "latency_ms=%.1f",
                         snap[i].provider, snap[i].base, code, ms);
    }
}

int ep_health_cold(ep_health_t *h, const char *provider, const char *model, double first_ms)
{
    if (!h || !provider)
        return 0;
    if (first_ms < EP_HEALTH_COLD_MS)
        return 0;

    /* 冷加载是"能力可用但代价高"的可观测信号：日志给出驻留建议，若该端点
     * 在探测视图内则一并记入，供 status 呈现。 */
    SVC_LOG_WARN("C-L02: EP-HEALTH: cold load provider=%s model=%s first_ms=%.0f "
                 "(hint: keep the model resident, e.g. OLLAMA_KEEP_ALIVE=30m)",
                 provider, model ? model : "-", first_ms);

    airy_mtx_lock(&h->lock);
    for (size_t i = 0; i < h->count; i++) {
        if (strcmp(h->recs[i].provider, provider) == 0) {
            h->recs[i].cold = 1;
            h->recs[i].cold_ms = first_ms;
            AIRY_STRNCPY_TERM(h->recs[i].cold_model, model ? model : "",
                              sizeof(h->recs[i].cold_model));
            break;
        }
    }
    airy_mtx_unlock(&h->lock);
    return 1;
}

char *ep_health_json(ep_health_t *h)
{
    if (!h)
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;

    airy_mtx_lock(&h->lock);
    for (size_t i = 0; i < h->count; i++) {
        const ep_rec_t *rec = &h->recs[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj)
            continue;

        cJSON_AddStringToObject(obj, "provider", rec->provider);
        cJSON_AddStringToObject(obj, "base", rec->base);
        cJSON_AddStringToObject(obj, "status", rec->reachable < 0
                                                    ? "unknown"
                                                    : (rec->reachable ? "ok" : "unreachable"));
        cJSON_AddNumberToObject(obj, "http_code", (double)rec->http_code);
        cJSON_AddNumberToObject(obj, "latency_ms", rec->latency_ms);
        cJSON_AddNumberToObject(obj, "cold", rec->cold ? 1 : 0);
        if (rec->cold) {
            cJSON_AddStringToObject(obj, "cold_model", rec->cold_model);
            cJSON_AddNumberToObject(obj, "first_ms", rec->cold_ms);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    int empty = (h->count == 0);
    airy_mtx_unlock(&h->lock);

    if (empty) {
        cJSON_Delete(arr);
        return NULL;
    }

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}
