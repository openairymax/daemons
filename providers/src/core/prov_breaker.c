// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file prov_breaker.c
 * @brief Provider 域调用侧熔断实现（0.1.21 B-4 / C-3）。
 *
 * 机制与策略分离：阈值、冷却窗口由策略层经 prov_brk_cfg()/环境变量注入；
 * 本件只回答「该 provider 此刻是否放行」并记账，不做任何路由选择。
 *
 * 状态机复用 commons cb_*（唯一 SSoT），本件不另建三态机；账目表只补
 * cb_* 不区分维度的两类口径：端点慢（计入连败）与预算不够（不计连败）。
 *
 * 并发：allow/ok/fail/tmo 在 daemon 请求线程，json/stat 在状态读取线程；
 * 全局表以 g.lock 串行化。锁内只做内存操作，不出网、不调用回调。
 */

#include "airy_memory.h"
#include "circuit_breaker.h"
#include "daemon_platform_ext.h"
#include "error.h"

#include "airy_prov/prov_breaker.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[PROV_BRK_NAME_MAX];
    circuit_breaker_t cb;
    prov_brk_stat_t st;
    uint32_t last; /* 上次观测到的 cb 状态，用于 opens 增量 */
} brk_rec_t;

typedef struct {
    airy_once_t once;
    airy_mtx_t lock;
    int booted;
    prov_brk_cfg_t cfg;
    cb_manager_t mgr;
    size_t count;
    brk_rec_t recs[PROV_BRK_MAX];
} brk_tbl_t;

static brk_tbl_t g_brk = {.once = AIRY_ONCE_INIT};

static uint32_t brk_env_u32(const char *key, uint32_t def)
{
    const char *v = getenv(key);
    if (!v || v[0] == '\0')
        return def;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v)
        return def;
    return (uint32_t)n;
}

void prov_brk_def(prov_brk_cfg_t *cfg)
{
    if (!cfg)
        return;
    cfg->fail_max = brk_env_u32("AIRY_PROV_BRK_FAIL_MAX", PROV_BRK_DEF_FAIL_MAX);
    cfg->open_ms = brk_env_u32("AIRY_PROV_BRK_OPEN_MS", PROV_BRK_DEF_OPEN_MS);
    cfg->half_max = brk_env_u32("AIRY_PROV_BRK_HALF_MAX", PROV_BRK_DEF_HALF_MAX);
}

static void brk_boot(void)
{
    airy_mtx_init(&g_brk.lock);
    prov_brk_def(&g_brk.cfg);
    g_brk.mgr = cb_manager_create();
    g_brk.booted = 1;
}

static int brk_ready(void)
{
    airy_call_once(&g_brk.once, brk_boot);
    return g_brk.booted;
}

/* 调用方持 g.lock。返回 NULL 仅当表满或建闸失败。 */
static brk_rec_t *brk_rec(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;
    for (size_t i = 0; i < g_brk.count; ++i) {
        if (strcmp(g_brk.recs[i].name, name) == 0)
            return &g_brk.recs[i];
    }
    if (g_brk.count >= PROV_BRK_MAX || !g_brk.mgr)
        return NULL;

    brk_rec_t *rec = &g_brk.recs[g_brk.count];
    cb_config_t c;
    AIRY_MEMSET(&c, 0, sizeof(c));
    c.failure_threshold = g_brk.cfg.fail_max;
    c.success_threshold = 1;
    c.timeout_ms = g_brk.cfg.open_ms;
    c.half_open_max_calls = g_brk.cfg.half_max;
    rec->cb = cb_create(g_brk.mgr, name, &c);
    if (!rec->cb)
        return NULL;
    AIRY_STRNCPY_TERM(rec->name, name, sizeof(rec->name));
    rec->last = PROV_BRK_CLOSED;
    g_brk.count++;
    return rec;
}

/* 调用方持 g.lock：只查不建，读面不因观察而扩张账目表。 */
static brk_rec_t *brk_find(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;
    for (size_t i = 0; i < g_brk.count; ++i) {
        if (strcmp(g_brk.recs[i].name, name) == 0)
            return &g_brk.recs[i];
    }
    return NULL;
}

/* 调用方持 g.lock：把 cb 真实状态并回账目（打开次数按跳变累计）。 */
static void brk_sync(brk_rec_t *rec)
{
    uint32_t cur = (uint32_t)cb_get_state(rec->cb);
    if (cur == PROV_BRK_OPEN && rec->last != PROV_BRK_OPEN)
        rec->st.opens++;
    rec->last = cur;
    rec->st.state = cur;
}

int prov_brk_cfg(const prov_brk_cfg_t *cfg)
{
    if (!brk_ready())
        return -1;
    prov_brk_cfg_t next;
    if (cfg)
        next = *cfg;
    else
        prov_brk_def(&next);

    airy_mtx_lock(&g_brk.lock);
    for (size_t i = 0; i < g_brk.count; ++i)
        cb_destroy(g_brk.recs[i].cb);
    AIRY_MEMSET(g_brk.recs, 0, sizeof(g_brk.recs));
    g_brk.count = 0;
    g_brk.cfg = next;
    airy_mtx_unlock(&g_brk.lock);
    return 1;
}

int prov_brk_allow(const char *provider)
{
    if (!brk_ready())
        return 1;
    if (!provider || provider[0] == '\0')
        return 1;

    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_rec(provider);
    if (!rec) {
        airy_mtx_unlock(&g_brk.lock);
        return 1;
    }
    /* fail_max=0：只记账、不放行闸门（现网行为不变）。 */
    if (g_brk.cfg.fail_max == 0) {
        rec->st.calls++;
        rec->st.state = PROV_BRK_CLOSED;
        airy_mtx_unlock(&g_brk.lock);
        return 1;
    }
    int pass = cb_allow_request(rec->cb) ? 1 : 0;
    brk_sync(rec);
    if (!pass) {
        rec->st.rejects++;
        rec->st.breaks = rec->st.rejects;
    } else {
        rec->st.calls++;
    }
    airy_mtx_unlock(&g_brk.lock);
    return pass;
}

void prov_brk_ok(const char *provider)
{
    if (!brk_ready())
        return;
    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_rec(provider);
    if (rec) {
        rec->st.oks++;
        cb_record_success(rec->cb, 0);
        brk_sync(rec);
    }
    airy_mtx_unlock(&g_brk.lock);
}

void prov_brk_fail(const char *provider, int err)
{
    if (!brk_ready())
        return;
    /* 调用方主动取消不是端点故障，不推进连败。 */
    if (err == AIRY_ERR_CANCELED)
        return;

    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_rec(provider);
    if (rec) {
        if (err == AIRY_ERR_TIMEOUT)
            rec->st.slow_tmo++;
        else
            rec->st.fails++;
        cb_record_failure(rec->cb, err);
        brk_sync(rec);
    }
    airy_mtx_unlock(&g_brk.lock);
}

void prov_brk_tmo(const char *provider, prov_tmo_t kind)
{
    if (!brk_ready())
        return;
    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_rec(provider);
    if (rec) {
        if (kind == PROV_TMO_TOTAL) {
            /* 整轮预算不够：记预算口径，不计连败（预算不是端点故障）。 */
            rec->st.budget_tmo++;
        } else {
            rec->st.slow_tmo++;
            cb_record_timeout(rec->cb);
        }
        brk_sync(rec);
    }
    airy_mtx_unlock(&g_brk.lock);
}

void prov_brk_reset(void)
{
    if (!brk_ready())
        return;
    airy_mtx_lock(&g_brk.lock);
    for (size_t i = 0; i < g_brk.count; ++i)
        cb_destroy(g_brk.recs[i].cb);
    AIRY_MEMSET(g_brk.recs, 0, sizeof(g_brk.recs));
    g_brk.count = 0;
    airy_mtx_unlock(&g_brk.lock);
}

const char *prov_brk_text(uint32_t state)
{
    switch (state) {
    case PROV_BRK_CLOSED:
        return "closed";
    case PROV_BRK_OPEN:
        return "open";
    case PROV_BRK_HALF:
        return "half_open";
    case PROV_BRK_NONE:
        return "none";
    default:
        return "unknown";
    }
}

int prov_brk_state(const char *provider)
{
    if (!brk_ready() || !provider || provider[0] == '\0')
        return PROV_BRK_NONE;

    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_find(provider);
    if (rec)
        brk_sync(rec); /* 读面回放 cb 真实状态，冷却到期的半开在此可见 */
    int state = rec ? (int)rec->st.state : PROV_BRK_NONE;
    airy_mtx_unlock(&g_brk.lock);
    return state;
}

int prov_brk_stat(const char *provider, prov_brk_stat_t *out)
{
    if (!out)
        return -1;
    if (!brk_ready() || !provider || provider[0] == '\0')
        return -1;

    airy_mtx_lock(&g_brk.lock);
    brk_rec_t *rec = brk_find(provider);
    if (!rec) {
        airy_mtx_unlock(&g_brk.lock);
        return -1;
    }
    brk_sync(rec);
    *out = rec->st;
    airy_mtx_unlock(&g_brk.lock);
    return 0;
}

char *prov_brk_json(void)
{
    if (!brk_ready())
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;

    airy_mtx_lock(&g_brk.lock);
    for (size_t i = 0; i < g_brk.count; i++) {
        brk_rec_t *rec = &g_brk.recs[i];
        brk_sync(rec);

        cJSON *obj = cJSON_CreateObject();
        if (!obj)
            continue;
        cJSON_AddStringToObject(obj, "provider", rec->name);
        cJSON_AddStringToObject(obj, "state", prov_brk_text(rec->st.state));
        cJSON_AddNumberToObject(obj, "calls", (double)rec->st.calls);
        cJSON_AddNumberToObject(obj, "oks", (double)rec->st.oks);
        cJSON_AddNumberToObject(obj, "fails", (double)rec->st.fails);
        cJSON_AddNumberToObject(obj, "slow_tmo", (double)rec->st.slow_tmo);
        cJSON_AddNumberToObject(obj, "budget_tmo", (double)rec->st.budget_tmo);
        cJSON_AddNumberToObject(obj, "rejects", (double)rec->st.rejects);
        cJSON_AddNumberToObject(obj, "opens", (double)rec->st.opens);
        cJSON_AddNumberToObject(obj, "breaks", (double)rec->st.breaks);
        cJSON_AddItemToArray(arr, obj);
    }
    airy_mtx_unlock(&g_brk.lock);

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

uint64_t prov_brk_breaks(void)
{
    if (!brk_ready())
        return 0;

    uint64_t sum = 0;
    airy_mtx_lock(&g_brk.lock);
    for (size_t i = 0; i < g_brk.count; ++i)
        sum += g_brk.recs[i].st.breaks;
    airy_mtx_unlock(&g_brk.lock);
    return sum;
}
