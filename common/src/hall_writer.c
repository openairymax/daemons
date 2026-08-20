// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file hall_writer.c
 * @brief Daemon-side hall event recording (write side).
 *
 * Shared writer used by daemon processes to record execution-chain events
 * (sched_d task lifecycle progress/result, tool_d tool execution results)
 * into the single-source-of-truth hall event store. Format is aligned
 * byte-for-byte with gateway_hall_store.c / hall_store.c: same root
 * (airy_data_dir()/agentrt/hall), same file naming
 * ({tenant}.{task}.{category}.{ts_utc}.{seq:04u}.json), same event body
 * header/access layout, same prev_file decision-chain linkage, same
 * write-then-read assertion (debug builds).
 */

// @owner: team-B
#include "hall_writer.h"

#include "airy_memory.h"
#include "airy_dirent.h"
#include "atomic_compat.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HW_TENANT "default"
#define HW_SCHEMA "task-file-v1"
#define HW_ROOT_REL "agentrt/hall"
#define HW_PATH_MAX 1024
#define HW_TS_LEN 24
#define HW_FILE_ID_MAX 192

#if defined(_WIN32)
#include <direct.h>
#define HW_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define HW_MKDIR(p) mkdir((p), 0755)
#endif

/* Lazy one-time init (thread-safe): 0=uninit, 2=initializing, 1=ready.
 * Daemon worker threads (sched_d worker / tool_d executor) call the writer
 * concurrently, so init and each write must be safe from races. */
static atomic_int g_hw_ready = 0;
static airy_mtx_t g_hw_lock;
static atomic_uint_fast64_t g_hw_gseq;

static void hw_ensure_init(void)
{
    while (atomic_load_explicit(&g_hw_ready, memory_order_acquire) != 1) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_hw_ready, &expected, 2,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            airy_mtx_init(&g_hw_lock);
            atomic_store_explicit(&g_hw_gseq, 0, memory_order_relaxed);
            atomic_store_explicit(&g_hw_ready, 1, memory_order_release);
            break;
        }
    }
}

static void hw_ts_utc(char *buf, size_t sz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &ts.tv_sec);
#else
    gmtime_r(&ts.tv_sec, &tmv);
#endif
    long ms = ts.tv_nsec / 1000000;
    snprintf(buf, sz, "%04d%02d%02dT%02d%02d%02d%03ld", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

static void hw_mkdirs(const char *path)
{
    char tmp[HW_PATH_MAX];
    AIRY_STRNCPY_TERM(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            HW_MKDIR(tmp);
            tmp[i] = saved;
        }
    }
    HW_MKDIR(tmp);
}

/* Scan one (task, category) dir for existing events. Returns the next seq
 * (max existing seq + 1) so concurrent writer processes (C CLI / gateway /
 * daemons) never overwrite each other's event files. When out_prev_file is
 * given it receives the file name of the current max-seq event, i.e. the
 * decision-chain predecessor for this (task, category).
 * seq is the second-to-last dot segment (tenant.task.cat.ts.seq.json),
 * same rule as hall_store.c hall_parse_seq. */
static unsigned hw_dir_scan(const char *dir, char *out_prev_file, size_t prev_sz)
{
    unsigned max_seq = 0;
    if (out_prev_file && prev_sz > 0)
        out_prev_file[0] = '\0';
    DIR *d = opendir(dir);
    if (!d)
        return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 5, ".json") != 0)
            continue;
        const char *last_dot = strrchr(n, '.');
        if (!last_dot || last_dot == n)
            continue;
        const char *dot2 = last_dot - 1;
        while (dot2 > n && *dot2 != '.')
            dot2--;
        if (*dot2 != '.')
            continue;
        unsigned seq = 0;
        int digits = 0;
        for (const char *q = dot2 + 1; q < last_dot; q++) {
            if (*q < '0' || *q > '9') {
                seq = 0;
                digits = 0;
                break;
            }
            seq = seq * 10 + (unsigned)(*q - '0');
            digits++;
        }
        if (digits > 0 && seq > max_seq) {
            max_seq = seq;
            if (out_prev_file && prev_sz > 0)
                AIRY_STRNCPY_TERM(out_prev_file, n, prev_sz);
        }
    }
    closedir(d);
    return max_seq + 1;
}

int daemon_hall_write(const char *task_id, const char *category, const char *node_id,
                      const char *content_json)
{
    if (!task_id || !task_id[0] || !category || !category[0] || !content_json || !content_json[0])
        return -1;

    hw_ensure_init();
    airy_mtx_lock(&g_hw_lock);

    uint64_t gseq = atomic_fetch_add_explicit(&g_hw_gseq, 1, memory_order_relaxed) + 1;

    char ts[HW_TS_LEN];
    hw_ts_utc(ts, sizeof(ts));

    char dir[HW_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s/%s", airy_data_dir(), HW_ROOT_REL, HW_TENANT,
             task_id, category);
    hw_mkdirs(dir);

    /* Decision-chain predecessor: the max-seq file in this (task, category)
     * dir ("" for the first event), mirroring hall_store.c prev_id. */
    char prev_file[HW_FILE_ID_MAX] = {0};
    unsigned seq = hw_dir_scan(dir, prev_file, sizeof(prev_file));

    char file_id[HW_FILE_ID_MAX];
    snprintf(file_id, sizeof(file_id), "%s.%s.%s.%s.%04u.json", HW_TENANT, task_id, category,
             ts, seq);

    /* Same write-role policy as hall_store.c: cognition-only files vs.
     * execution-class files writable by executors too. */
    const char *write_roles = (strcmp(category, "blueprint") == 0 ||
                               strcmp(category, "command") == 0 ||
                               strcmp(category, "chain") == 0) ?
                                  "[\"cognition\"]" :
                                  "[\"cognition\",\"executor\"]";

    char header[768];
    snprintf(header, sizeof(header),
             "{\"file\":{\"id\":\"%s\",\"category\":\"%s\",\"schema\":\"%s\","
             "\"tenant_id\":\"%s\",\"task_id\":\"%s\",\"node_id\":\"%s\","
             "\"ts_utc\":\"%s\",\"seq\":%u,\"gseq\":%llu,\"prev_file\":\"%s\"},"
             "\"access\":{\"owner_role\":\"cognition\",\"write_roles\":%s,"
             "\"read_roles\":[\"cognition\"]},\"content\":",
             file_id, category, HW_SCHEMA, HW_TENANT, task_id, node_id ? node_id : "", ts, seq,
             (unsigned long long)gseq, prev_file, write_roles);

    size_t json_len = strlen(header) + strlen(content_json) + 2;
    char *json = (char *)AIRY_MALLOC(json_len);
    if (!json) {
        airy_mtx_unlock(&g_hw_lock);
        return -1;
    }
    snprintf(json, json_len, "%s%s}", header, content_json);

    char path[HW_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, file_id);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        AIRY_FREE(json);
        airy_mtx_unlock(&g_hw_lock);
        SVC_LOG_WARN("hall_writer: write failed (path=%s)", path);
        return -1;
    }
    fputs(json, fp);
    fclose(fp);
    AIRY_FREE(json);
    airy_mtx_unlock(&g_hw_lock);

#ifndef NDEBUG
    /* 单一真相源事件流断言（与 hall_store.c S-6 对齐）：写后必可读。
     * 事件文件必须能立即重新打开并包含自身 file.id，否则事件流与持久化
     * 不一致（记录丢失）。file.id 位于 header 开头，读取前 1KB 足够。 */
    {
        FILE *rf = fopen(path, "r");
        if (rf) {
            char buf[1024];
            size_t got = fread(buf, 1, sizeof(buf) - 1, rf);
            buf[got] = '\0';
            fclose(rf);
            if (strstr(buf, file_id) == NULL) {
                SVC_LOG_ERROR("hall_writer: invariant violated - write-then-read mismatch for %s",
                              file_id);
                return -1;
            }
        } else {
            SVC_LOG_ERROR("hall_writer: invariant violated - event file missing after write (%s)",
                          path);
            return -1;
        }
    }
#endif

    return 0;
}
