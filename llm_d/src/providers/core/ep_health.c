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
 * 深度探针（D2-9⑤）：轻探测（/models）只证明路由存在，1-token 生成才
 * 证明权重已加载、推理可用。深度探针同样由策略层按倍率触发，本件不自持
 * 周期；结论粘滞到下一次深度探测，避免"路由在但推理挂"被轻探测掩盖。
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
#include "provider_http.h"

#include <cjson/cJSON.h>
#include <string.h>

#define EP_HEALTH_MAX_ENDPOINTS 16
#define EP_HEALTH_PROBE_TIMEOUT_SEC 8.0
#define EP_HEALTH_DEEP_TIMEOUT_SEC 60.0
#define EP_HEALTH_COLD_MS 10000.0
#define EP_HEALTH_DEFAULT_PATH "/models"

typedef struct {
    char provider[64];
    /* 出网快照：provider_base_ctx_t 无指针成员，值拷贝后 registry 销毁
     * 也不产生悬垂引用（api_base/api_key/预算/重试策略一并在内）。 */
    provider_base_ctx_t ctx;
    /* 深度探针三要素：模型名、生成路径、鉴权头声明。headers 指向适配器
     * 静态表，进程生命期有效；任一缺失则深度探针不适用。 */
    char model[128];
    char chat_path[64];
    const provider_header_spec_t *headers;
    int reachable; /* -1 未知 / 0 不可达 / 1 可达 */
    int deep;      /* -1 未探 / 0 失败 / 1 通过；结果粘滞到下一次深度探测 */
    long http_code;
    double latency_ms;
    int cold;
    char cold_model[64];
    double cold_ms;
} ep_rec_t;

/* 探测快照：只携带出网所需字段，逐端点拷出后在锁外使用。 */
typedef struct {
    char provider[64];
    provider_base_ctx_t ctx;
    char model[128];
    char chat_path[64];
    const provider_header_spec_t *headers;
} ep_probe_t;

struct ep_health {
    char filter[64];
    char probe_path[64];
    airy_mtx_t lock;
    size_t count;
    ep_health_sink_t sink;
    void *sink_ud;
    ep_rec_t recs[EP_HEALTH_MAX_ENDPOINTS];
};

/* filter 是探测范围策略键（svc.c 注入）："local" = 端点 api_base 指向本机
 * loopback。位置是客观网络事实，不耦合 provider 用户命名（如
 * "local-llamacpp"）——对命名规律做前缀猜测属策略混入机制。空 = 全部；
 * 未知键 fail-safe 判不命中（宁可不探，不意外出网）。 */
static int ep_host_is_local(const char *host)
{
    static const char *const kLocal[] = {"127.0.0.1", "localhost", "[::1]"};
    for (size_t i = 0; i < sizeof(kLocal) / sizeof(kLocal[0]); ++i) {
        size_t n = strlen(kLocal[i]);
        if (strncmp(host, kLocal[i], n) != 0)
            continue;
        char c = host[n];
        if (c == '\0' || c == ':' || c == '/' || c == '?')
            return 1;
    }
    return 0;
}

static int ep_probe_wanted(const char *filter, const char *api_base)
{
    if (filter[0] == '\0')
        return 1;
    if (strcmp(filter, "local") != 0 || !api_base)
        return 0;
    const char *host = strstr(api_base, "://");
    return ep_host_is_local(host ? host + 3 : api_base);
}

static int ep_snap_cb(const provider_t *prov, void *ud)
{
    ep_health_t *h = (ep_health_t *)ud;
    if (!prov || !prov->name)
        return 0;
    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    if (!ep_probe_wanted(h->filter, base ? base->api_base : NULL))
        return 0;

    if (h->count >= EP_HEALTH_MAX_ENDPOINTS) {
        SVC_LOG_WARN("C-L02: EP-HEALTH: endpoint view full at %d, remaining endpoints ignored",
                     EP_HEALTH_MAX_ENDPOINTS);
        return 1;
    }

    ep_rec_t *rec = &h->recs[h->count];
    AIRY_STRNCPY_TERM(rec->provider, prov->name, sizeof(rec->provider));

    if (base)
        rec->ctx = *base;

    /* 深度探针要素：优先取注册表首个模型名（端点真实可服务的模型），
     * 回落到适配器默认名；生成路径与头声明取自适配器静态表。 */
    const char *model = NULL;
    if (prov->models && prov->models[0] && prov->models[0][0])
        model = prov->models[0];
    else if (prov->adapter && prov->adapter->default_model)
        model = prov->adapter->default_model;
    if (model)
        AIRY_STRNCPY_TERM(rec->model, model, sizeof(rec->model));
    if (prov->adapter) {
        if (prov->adapter->chat_path)
            AIRY_STRNCPY_TERM(rec->chat_path, prov->adapter->chat_path, sizeof(rec->chat_path));
        rec->headers = prov->adapter->headers;
    }

    rec->reachable = -1;
    rec->deep = -1;
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

void ep_health_sink(ep_health_t *h, ep_health_sink_t sink, void *ud)
{
    if (!h)
        return;

    /* 装配域在启动期一次性接线；运行期只读，无需额外同步。 */
    airy_mtx_lock(&h->lock);
    h->sink = sink;
    h->sink_ud = ud;
    airy_mtx_unlock(&h->lock);
}

static void ep_probe_one(const ep_probe_t *p, const char *path, int *out_ok, long *out_code,
                         double *out_ms)
{
    *out_ok = 0;
    *out_code = 0;
    *out_ms = 0.0;

    uint64_t t0 = airy_time_ms();
    provider_http_resp_t *resp = NULL;
    long code = 0;
    int rc = prov_probe_get(&p->ctx, path, p->headers, EP_HEALTH_PROBE_TIMEOUT_SEC, &resp, &code);
    double ms = (double)(airy_time_ms() - t0);
    provider_http_resp_free(resp);

    *out_code = code;
    *out_ms = ms;
    *out_ok = (rc == AIRY_OK && code >= 200 && code < 400);
}

/* 深度探针请求体：1-token 生成。1 token 足以证明权重已加载、KV 可分配、
 * 采样链可出字；stream=false 让端点以完整 JSON 应答，按状态码即可判定。 */
static char *ep_deep_body(const char *model)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddStringToObject(root, "model", model);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (!msgs) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *msg = cJSON_CreateObject();
    if (!msg) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "ping");
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddNumberToObject(root, "max_tokens", 1);
    cJSON_AddBoolToObject(root, "stream", 0);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* 深度探针：走与业务请求同一条出网门面（鉴权头/预算/错误归一一致），
 * 只是载荷固定为 1-token 生成；失败形态的处置仍由出网门面统一裁决。 */
static void ep_probe_deep(const ep_probe_t *p, int *out_ok, long *out_code, double *out_ms)
{
    *out_ok = 0;
    *out_code = 0;
    *out_ms = 0.0;
    if (!p->model[0] || !p->chat_path[0])
        return;

    char *body = ep_deep_body(p->model);
    if (!body)
        return;

    /* exec 只读 base（不刷新密钥、不写字段），const 转型安全。 */
    provider_request_t req = {
        .base = (provider_base_ctx_t *)&p->ctx,
        .path = p->chat_path,
        .headers = p->headers,
        .body = body,
        .timeout_sec = EP_HEALTH_DEEP_TIMEOUT_SEC,
        .idempotent = 1,
    };

    uint64_t t0 = airy_time_ms();
    provider_http_resp_t *resp = NULL;
    long code = 0;
    int rc = provider_http_exec(&req, &resp, &code);
    *out_ms = (double)(airy_time_ms() - t0);
    provider_http_resp_free(resp);
    cJSON_free(body);

    *out_code = code;
    *out_ok = (rc == AIRY_OK && code == 200);
}

void ep_health_sweep(ep_health_t *h, int deep)
{
    if (!h)
        return;

    /* 锁内取探测快照，锁外出网：任何网络等待都不得阻塞 cold/json 的读侧。 */
    ep_probe_t snap[EP_HEALTH_MAX_ENDPOINTS];
    size_t n;
    airy_mtx_lock(&h->lock);
    n = h->count;
    for (size_t i = 0; i < n; i++) {
        const ep_rec_t *rec = &h->recs[i];
        AIRY_STRNCPY_TERM(snap[i].provider, rec->provider, sizeof(snap[i].provider));
        snap[i].ctx = rec->ctx;
        AIRY_STRNCPY_TERM(snap[i].model, rec->model, sizeof(snap[i].model));
        AIRY_STRNCPY_TERM(snap[i].chat_path, rec->chat_path, sizeof(snap[i].chat_path));
        snap[i].headers = rec->headers;
    }
    airy_mtx_unlock(&h->lock);

    for (size_t i = 0; i < n; i++) {
        int ok = 0;
        long code = 0;
        double ms = 0.0;
        ep_probe_one(&snap[i], h->probe_path, &ok, &code, &ms);

        int deep_ok = 1;
        int deep_ran = 0;
        long deep_code = 0;
        double deep_ms = 0.0;
        if (deep && snap[i].model[0] && snap[i].chat_path[0]) {
            ep_probe_deep(&snap[i], &deep_ok, &deep_code, &deep_ms);
            deep_ran = 1;
        }

        airy_mtx_lock(&h->lock);
        int found = 0;
        int reached = 0;
        for (size_t j = 0; j < h->count; j++) {
            ep_rec_t *rec = &h->recs[j];
            if (strcmp(rec->provider, snap[i].provider) != 0)
                continue;
            /* 只刷新可达性与深度结论；cold 字段由请求侧独立维护。 */
            rec->latency_ms = ms;
            if (deep_ran)
                rec->deep = deep_ok;
            /* 粘滞判定：轻探测通过但最近一次深度探测失败，端点仍判不可达，
             * 直到下一次深度探测恢复——"路由在但推理挂"不得报 ok。 */
            rec->reachable = (ok && rec->deep != 0) ? 1 : 0;
            rec->http_code = (deep_ran && ok && !deep_ok) ? deep_code : code;
            found = 1;
            reached = rec->reachable;
            break;
        }
        ep_health_sink_t sink = h->sink;
        void *sink_ud = h->sink_ud;
        airy_mtx_unlock(&h->lock);

        /* 汇聚回调必须在锁外：sink 内会取上层（router）锁，持锁回调即构成
         * 跨层锁序，违反"绝不出锁调用外部"的并发纪律。 */
        if (found && sink)
            sink(sink_ud, snap[i].provider, reached);

        if (!ok)
            SVC_LOG_WARN("C-L02: EP-HEALTH: probe FAIL provider=%s base=%s http=%ld "
                         "latency_ms=%.1f",
                         snap[i].provider, snap[i].ctx.api_base, code, ms);
        else if (deep_ran && !deep_ok)
            SVC_LOG_WARN("C-L02: EP-HEALTH: deep probe FAIL provider=%s model=%s http=%ld "
                         "latency_ms=%.1f",
                         snap[i].provider, snap[i].model, deep_code, deep_ms);
        else
            SVC_LOG_DEBUG("C-L02: EP-HEALTH: probe ok provider=%s http=%ld latency_ms=%.1f deep=%d",
                          snap[i].provider, code, ms, deep_ran ? deep_ok : -1);
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
        cJSON_AddStringToObject(obj, "base", rec->ctx.api_base);
        cJSON_AddStringToObject(obj, "status", rec->reachable < 0
                                                    ? "unknown"
                                                    : (rec->reachable ? "ok" : "unreachable"));
        cJSON_AddNumberToObject(obj, "http_code", (double)rec->http_code);
        cJSON_AddNumberToObject(obj, "latency_ms", rec->latency_ms);
        cJSON_AddNumberToObject(obj, "deep", (double)rec->deep);
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
