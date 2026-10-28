// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file bench_cost_hotpath.c
 * @brief 计费热路径两臂基准（0.1.20 Gate 37 证据源）
 *
 * 单请求粒度对比两条路径：
 *   A 臂  add + estimate + flush —— 即生产热路径 update_cost_tracking() 的形态
 *   B 臂  add + estimate         —— 同一账户去掉持久化，作为对照
 * 每请求以 airy_time_ns() 采样耗时，并以符号介入统计该请求内真实落盘的次数。
 *
 * fsync_probe 用一次 cost_tracker_save() 打穿落盘路径：计数 ≥1 才说明介入器
 * 真挂在 fsync/_commit 上；否则 A 臂的「零 fsync」只是测量失明，不是稳态证据。
 *
 * 本程序只测量并输出证据，不判断阈值——裁决属门禁。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cost_tracker.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#define BPH_N 512
#define BPH_WARM 16

/* 真实落盘调用计数：A/B 两臂共用，按请求取差分。 */
static long g_sync_hits;

#if defined(_WIN32)
typedef int (*bph_commit_fn)(int);

int _commit(int fd)
{
    static bph_commit_fn real_fn;
    static int resolved;

    if (!resolved) {
        resolved = 1;
        HMODULE crt = GetModuleHandleA("ucrtbase.dll");
        if (!crt)
            crt = GetModuleHandleA("msvcrt.dll");
        if (crt)
            real_fn = (bph_commit_fn)(void *)GetProcAddress(crt, "_commit");
    }
    ++g_sync_hits;
    return real_fn ? real_fn(fd) : -1;
}
#else
int fsync(int fd)
{
    static int (*real_fn)(int);

    if (!real_fn) {
        void *sym = dlsym(RTLD_NEXT, "fsync");
        /* ISO C 不允许对象指针与函数指针互转，经 memcpy 保持位模式。 */
        memcpy(&real_fn, &sym, sizeof(real_fn));
    }
    ++g_sync_hits;
    return real_fn ? real_fn(fd) : -1;
}
#endif

typedef struct {
    long sync_total;
    long sync_max;
    double p50_us;
    double p95_us;
    double p99_us;
    double max_us;
} bph_arm_t;

static double g_smp[BPH_N];

static int bph_cmp(const void *lhs, const void *rhs)
{
    double a = *(const double *)lhs;
    double b = *(const double *)rhs;
    return (a > b) - (a < b);
}

static double bph_pct(const double *v, int n, double q)
{
    int i = (int)((double)n * q);
    if (i >= n)
        i = n - 1;
    return v[i];
}

static void bph_stats(int n, bph_arm_t *out)
{
    qsort(g_smp, (size_t)n, sizeof(g_smp[0]), bph_cmp);
    out->p50_us = bph_pct(g_smp, n, 0.50);
    out->p95_us = bph_pct(g_smp, n, 0.95);
    out->p99_us = bph_pct(g_smp, n, 0.99);
    out->max_us = g_smp[n - 1];
}

static void bph_request(cost_tracker_t *ct, const char *model, const char *path, int persist)
{
    cost_tracker_add(ct, model, 128u, 64u, 0u);
    (void)cost_tracker_estimate(ct, model, 128u, 64u);
    if (persist)
        (void)cost_tracker_flush(ct, path);
}

static void bph_arm(cost_tracker_t *ct, const char *path, int persist, bph_arm_t *out)
{
    static const char *const models[] = {"gpt-4o-mini", "deepseek-chat", "local-default"};
    const int nm = (int)(sizeof(models) / sizeof(models[0]));

    memset(out, 0, sizeof(*out));

    for (int i = 0; i < BPH_WARM; ++i)
        bph_request(ct, models[i % nm], path, persist);

    for (int i = 0; i < BPH_N; ++i) {
        const char *model = models[i % nm];
        long before = g_sync_hits;
        double t0 = (double)airy_time_ns();

        bph_request(ct, model, path, persist);

        g_smp[i] = ((double)airy_time_ns() - t0) / 1000.0;
        long delta = g_sync_hits - before;
        out->sync_total += delta;
        if (delta > out->sync_max)
            out->sync_max = delta;
    }
    bph_stats(BPH_N, out);
}

static int bph_tmp(char *buf, size_t cap, const char *tag)
{
    const char *dir = airy_tmp_dir();
    if (!dir || !dir[0])
        dir = getenv("TMPDIR");
    if (!dir || !dir[0])
        dir = ".";
#if defined(_WIN32)
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    int n = snprintf(buf, cap, "%s/g37-cost-%s-%ld.json", dir, tag, pid);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int bph_artifact(char *buf, size_t cap)
{
    const char *path = getenv("G37_COST_ARTIFACT");
    if (path && path[0])
        return snprintf(buf, cap, "%s", path) >= (int)cap ? -1 : 0;
    return bph_tmp(buf, cap, "hotpath");
}

static void bph_drop(const char *path)
{
    char tmp[1024];
    remove(path);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) > 0)
        remove(tmp);
}

int main(void)
{
    static const pricing_rule_t rules[] = {
        {"gpt-4o-mini", 0.15, 0.60},
        {"deepseek*", 0.14, 0.28},
    };
    const int rule_count = (int)(sizeof(rules) / sizeof(rules[0]));

    char art[512], pa[512], pb[512];
    if (bph_artifact(art, sizeof(art)) != 0 || bph_tmp(pa, sizeof(pa), "a") != 0 ||
        bph_tmp(pb, sizeof(pb), "b") != 0) {
        fprintf(stderr, "bench_cost_hotpath: cannot resolve evidence paths\n");
        return 1;
    }

    cost_tracker_t *arm_a = cost_tracker_create(rules, rule_count);
    cost_tracker_t *arm_b = cost_tracker_create(rules, rule_count);
    if (!arm_a || !arm_b) {
        fprintf(stderr, "bench_cost_hotpath: cannot create tracker\n");
        cost_tracker_destroy(arm_a);
        cost_tracker_destroy(arm_b);
        return 1;
    }

    bph_arm_t a, b;
    bph_arm(arm_a, pa, 1, &a);
    bph_arm(arm_b, pb, 0, &b);

    /* 介入器探针：退出路径的强同步必须被计数看到。 */
    long before = g_sync_hits;
    (void)cost_tracker_save(arm_a, pa);
    long probe = g_sync_hits - before;

    double delta = a.p99_us - b.p99_us;

    FILE *f = fopen(art, "wb");
    if (!f) {
        fprintf(stderr, "bench_cost_hotpath: cannot write %s\n", art);
        cost_tracker_destroy(arm_a);
        cost_tracker_destroy(arm_b);
        bph_drop(pa);
        bph_drop(pb);
        return 1;
    }
    fprintf(f,
            "{\"n_a\":%d,\"n_b\":%d,\"warm\":%d,"
            "\"a\":{\"sync_total\":%ld,\"sync_max\":%ld,\"p50_us\":%.3f,"
            "\"p95_us\":%.3f,\"p99_us\":%.3f,\"max_us\":%.3f},"
            "\"b\":{\"sync_total\":%ld,\"sync_max\":%ld,\"p50_us\":%.3f,"
            "\"p95_us\":%.3f,\"p99_us\":%.3f,\"max_us\":%.3f},"
            "\"p99_delta_us\":%.3f,\"fsync_probe\":%ld}\n",
            BPH_N, BPH_N, BPH_WARM, a.sync_total, a.sync_max, a.p50_us, a.p95_us, a.p99_us,
            a.max_us, b.sync_total, b.sync_max, b.p50_us, b.p95_us, b.p99_us, b.max_us, delta,
            probe);
    fclose(f);

    printf("cost hotpath: n=%d sync_a=%ld(max %ld) p99_a=%.1fus | sync_b=%ld p99_b=%.1fus | "
           "delta=%+.1fus probe=%ld\n",
           BPH_N, a.sync_total, a.sync_max, a.p99_us, b.sync_total, b.p99_us, delta, probe);
    printf("evidence: %s\n", art);

    cost_tracker_destroy(arm_a);
    cost_tracker_destroy(arm_b);
    bph_drop(pa);
    bph_drop(pb);

    if (probe < 1) {
        fprintf(stderr, "bench_cost_hotpath: sync interposer saw no fsync; measurement is blind\n");
        return 1;
    }
    return 0;
}
