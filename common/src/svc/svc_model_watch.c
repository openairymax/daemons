/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_model_watch.c
 * @brief model.yaml 启动快照 + 请求侧变更检测实现（见 svc_model_watch.h）。
 *
 * 平台件复用：airy_file_stat（跨平台元数据）/ airy_io_read_file（整读）/
 * airy_runtime_dir（运行时目录）。标记文件 model_yaml_stale_<daemon>
 * 仅作存在性信号，启动器 status 面按 glob 聚合；启动时清除，防陈旧信号。
 */

#include "svc_model_watch.h"

#include <stdio.h>

#include "airy_memory_inline.h"
#include "io.h"
#include "platform_misc.h"
#include "platform_paths.h"
#include "svc_logger.h"

#define MW_MARKER_FMT "%s/model_yaml_stale_%s"

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

static int mw_probe(svc_mw_t *w, uint64_t *size, int64_t *sec, int64_t *nsec,
                    uint64_t *hash)
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

static void mw_marker_path(const svc_mw_t *w, char *buf, size_t cap)
{
    snprintf(buf, cap, MW_MARKER_FMT, airy_runtime_dir(), w->daemon);
}

int svc_mw_init(svc_mw_t *w, const char *path, const char *daemon)
{
    if (!w || !path || !path[0] || !daemon || !daemon[0])
        return -1;

    AIRY_MEMSET(w, 0, sizeof(*w));
    snprintf(w->path, sizeof(w->path), "%s", path);
    snprintf(w->daemon, sizeof(w->daemon), "%s", daemon);

    if (mw_probe(w, &w->size, &w->mtime_sec, &w->mtime_nsec, &w->hash) != 0) {
        w->valid = 0;
        return -1;
    }
    w->valid = 1;

    /* 启动即新鲜：清本 daemon 陈旧标记（他人标记不动） */
    char marker[1024];
    mw_marker_path(w, marker, sizeof(marker));
    remove(marker);
    return 0;
}

int svc_mw_check(svc_mw_t *w)
{
    if (!w || !w->valid)
        return -1;

    airy_file_stat_t st;
    if (airy_file_stat(w->path, &st) != 0)
        st.exists = false;

    if (!st.exists)
        goto drifted; /* 配置文件被删亦属漂移 */
    if (st.size == w->size && st.mtime_sec == w->mtime_sec &&
        st.mtime_nsec == w->mtime_nsec)
        return 0; /* 廉价路径：元数据未变，不读内容 */

    uint64_t size = 0, hash = 0;
    int64_t sec = 0, nsec = 0;
    if (mw_probe(w, &size, &sec, &nsec, &hash) != 0)
        goto drifted;
    if (size == w->size && hash == w->hash) {
        /* touch 误报消解：内容一致，仅刷新元数据基线 */
        w->mtime_sec = sec;
        w->mtime_nsec = nsec;
        return 0;
    }

drifted:
    if (!w->warned) {
        w->warned = 1;
        SVC_LOG_WARN("model.yaml changed since startup (daemon=%s path=%s) — "
                     "restart to apply",
                     w->daemon, w->path);
        char marker[1024];
        mw_marker_path(w, marker, sizeof(marker));
        FILE *f = fopen(marker, "w");
        if (f) {
            fprintf(f, "%s\n", w->daemon);
            fclose(f);
        }
    }
    return 1;
}
