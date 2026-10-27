/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_model_watch.c
 * @brief model.yaml 启动快照、漂移检测与应用入口实现（见 svc_model_watch.h）。
 *
 * 平台件复用：airy_file_stat（跨平台元数据）/ airy_io_read_file（整读）/
 * airy_runtime_dir（运行时目录）。标记文件 model_yaml_stale_<daemon>
 * 仅作存在性信号，启动器 status 面按 glob 聚合；启动/应用成功即清除，
 * 防陈旧信号。
 *
 * 事件经 hall_evt_write 上报，复用 daemon_dep.c 的 envelope 范式；
 * hash 以 16 位十六进制字符串下发，避免 cJSON 数值走 double 丢精度。
 */

#include "svc_model_watch.h"

#include <stdio.h>

#include <cjson/cJSON.h>

#include "airy_memory_inline.h"
#include "hall_event.h"
#include "io.h"
#include "platform_misc.h"
#include "platform_paths.h"
#include "svc_logger.h"

#define MW_MARKER_FMT "%s/model_yaml_stale_%s"
#define MW_WHY_MAX 160

/* FNV-1a 64：变更检测用非加密摘要 */
static uint64_t mw_hash_buf(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int mw_probe(const svc_mw_t *w, uint64_t *size, int64_t *sec,
                    int64_t *nsec, uint64_t *hash)
{
    airy_file_stat_t st;
    if (airy_file_stat(w->path, &st) != 0 || !st.exists || !st.is_regular)
        return -1;
    char *buf = NULL;
    size_t len = 0;
    if (airy_io_read_file(w->path, &buf, &len) != 0 || !buf)
        return -1;
    *hash = mw_hash_buf(buf, len);
    AIRY_FREE(buf);
    *size = st.size;
    *sec = st.mtime_sec;
    *nsec = st.mtime_nsec;
    return 0;
}

static void mw_marker(const svc_mw_t *w, char *buf, size_t cap)
{
    snprintf(buf, cap, MW_MARKER_FMT, airy_runtime_dir(), w->daemon);
}

static void mw_mark_clear(const svc_mw_t *w)
{
    if (!w->daemon[0])
        return;
    char marker[1024];
    mw_marker(w, marker, sizeof(marker));
    remove(marker);
}

static void mw_mark_set(const svc_mw_t *w)
{
    char marker[1024];
    mw_marker(w, marker, sizeof(marker));
    FILE *f = fopen(marker, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", w->daemon);
    fclose(f);
}

/* 一次性闩锁只在漂移消解/重照快照时复位，允许后续漂移再次上报 */
static void mw_clear(svc_mw_t *w)
{
    w->warned = 0;
    w->drift_ev = 0;
    w->drift_pending = 0;
}

/**
 * 统一的漂移判定。
 * @return 1 = 漂移（*nh 为新摘要，文件缺失时为 0）；0 = 无漂移；<0 = 监视停用
 */
static int mw_detect(svc_mw_t *w, uint64_t *nh, int *absent)
{
    if (!w || !w->valid || !w->path[0])
        return -1;

    airy_file_stat_t st;
    if (airy_file_stat(w->path, &st) != 0)
        st.exists = false;

    if (!st.exists) {
        /* 配置文件被删亦属漂移 */
        mw_clear(w);
        w->drift_pending = 1;
        if (nh)
            *nh = 0;
        if (absent)
            *absent = 1;
        return 1;
    }
    if (st.size == w->size && st.mtime_sec == w->mtime_sec &&
        st.mtime_nsec == w->mtime_nsec) {
        mw_clear(w); /* 廉价路径：元数据未变，不读内容 */
        return 0;
    }

    uint64_t size = 0, hash = 0;
    int64_t sec = 0, nsec = 0;
    if (mw_probe(w, &size, &sec, &nsec, &hash) == 0 &&
        size == w->size && hash == w->hash) {
        /* touch 误报消解：内容一致，仅刷新元数据基线 */
        w->mtime_sec = sec;
        w->mtime_nsec = nsec;
        mw_clear(w);
        return 0;
    }

    if (nh)
        *nh = hash;
    if (absent)
        *absent = 0;
    w->drift_pending = 1;
    return 1;
}

static void mw_put_hash(cJSON *o, const char *key, uint64_t h)
{
    char buf[20];
    if (h)
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    else
        buf[0] = '\0';
    cJSON_AddStringToObject(o, key, buf);
}

static void mw_emit(const svc_mw_t *w, cJSON *evt)
{
    char *json = cJSON_PrintUnformatted(evt);
    if (json) {
        (void)hall_evt_write(w->daemon, "issue", NULL, json);
        cJSON_free(json);
    }
    cJSON_Delete(evt);
}

static void mw_ev_drift(svc_mw_t *w, uint64_t nh)
{
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "config_drift");
    cJSON_AddStringToObject(evt, "file", w->path);
    mw_put_hash(evt, "old_hash", w->hash);
    mw_put_hash(evt, "new_hash", nh);
    cJSON_AddBoolToObject(evt, "applied", 0);
    mw_emit(w, evt);
}

static void mw_ev_fail(svc_mw_t *w, const char *why)
{
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "config_reload_failed");
    cJSON_AddStringToObject(evt, "file", w->path);
    cJSON_AddStringToObject(evt, "reason",
                            (why && why[0]) ? why : "unspecified");
    mw_emit(w, evt);
}

static void mw_ev_apply(svc_mw_t *w)
{
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "config_applied");
    cJSON_AddStringToObject(evt, "file", w->path);
    mw_put_hash(evt, "hash", w->hash);
    cJSON_AddNumberToObject(evt, "generation", (double)w->generation);
    mw_emit(w, evt);
}

int svc_mw_init(svc_mw_t *w, const char *path, const char *daemon)
{
    if (!w || !path || !path[0] || !daemon || !daemon[0])
        return -1;

    AIRY_MEMSET(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);
    snprintf(w->daemon, sizeof(w->daemon), "%s", daemon);
    return svc_mw_rebase(w, NULL);
}

int svc_mw_check(svc_mw_t *w)
{
    uint64_t nh = 0;
    int absent = 0;
    int rc = mw_detect(w, &nh, &absent);
    if (rc <= 0)
        return rc;

    if (!w->warned) {
        w->warned = 1;
        SVC_LOG_WARN("model.yaml changed since startup (daemon=%s path=%s) — "
                     "SIGHUP to apply",
                     w->daemon, w->path);
        mw_mark_set(w);
    }
    return 1;
}

int svc_mw_hook(svc_mw_t *w, svc_mw_apply_t apply, void *ud,
                uint32_t interval_ms)
{
    if (!w || !apply)
        return -1;
    w->apply = apply;
    w->apply_ud = ud;
    w->interval_ms = interval_ms;
    return 0;
}

int svc_mw_tick(svc_mw_t *w, uint64_t now_ms)
{
    if (!w || !w->valid)
        return -1;

    uint32_t period = w->interval_ms ? w->interval_ms : SVC_MW_INTERVAL_MS;
    if (w->last_ms != 0 && now_ms > w->last_ms &&
        now_ms - w->last_ms < period)
        return 0;
    w->last_ms = now_ms;

    uint64_t nh = 0;
    int absent = 0;
    int rc = mw_detect(w, &nh, &absent);
    if (rc <= 0)
        return rc;

    if (!w->drift_ev) {
        w->drift_ev = 1;
        mw_ev_drift(w, nh);
        SVC_LOG_WARN("config drift detected (daemon=%s path=%s) — "
                     "SIGHUP to apply",
                     w->daemon, w->path);
    }
    return 1;
}

int svc_mw_reload(svc_mw_t *w)
{
    if (!w || !w->apply)
        return -1;

    char why[MW_WHY_MAX];
    why[0] = '\0';

    int rc = w->apply(w->apply_ud, why, sizeof(why));
    if (rc != 0) {
        if (rc < 0)
            mw_ev_fail(w, why);
        return rc;
    }

    if (svc_mw_rebase(w, NULL) != 0)
        SVC_LOG_WARN("config snapshot unavailable after reload "
                     "(daemon=%s path=%s)",
                     w->daemon, w->path);
    w->generation++;
    mw_ev_apply(w);
    return 0;
}

int svc_mw_rebase(svc_mw_t *w, const char *path)
{
    if (!w)
        return -1;

    if (path && path != w->path) {
        if (!path[0]) {
            w->path[0] = '\0';
            w->valid = 0;
            mw_clear(w);
            return 0;
        }
        snprintf(w->path, sizeof(w->path), "%s", path);
    }
    if (!w->path[0]) {
        w->valid = 0;
        mw_clear(w);
        return 0;
    }

    uint64_t size = 0, hash = 0;
    int64_t sec = 0, nsec = 0;
    if (mw_probe(w, &size, &sec, &nsec, &hash) != 0) {
        w->valid = 0;
        return -1;
    }

    w->size = size;
    w->mtime_sec = sec;
    w->mtime_nsec = nsec;
    w->hash = hash;
    w->valid = 1;
    mw_clear(w);
    mw_mark_clear(w);
    return 0;
}
