// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file bench_gateway_pep.c
 * @brief gateway PEP 裁定路径端到端基准（M2-S1，0.1.9 §3.4 前置）。
 *
 * 为 cupolas PDP 化（M2）建立 gateway PEP 端到端基线：
 *   - 冷路径（miss）：唯一 key 首次检查 → 一次 check_permission RPC 往返
 *   - 热路径（hit）：同 key 重复检查 → 缓存命中，零 RPC
 *   - epoch 失效：策略热更新（epoch+1）后缓存整体失效与重填成本
 *   - 混合负载：80% 热 key + 20% 冷 key 的命中率（RPC 计数）
 *
 * 用假 PDP（unix socket 服务端线程）模拟 cupolas_d check_permission，
 * 与 test_gateway_pep_cache 同一框架（语义断言见该测试，本文件只测
 * 延迟/命中率基线，不做性能断言以免 CI 抖动）。
 *
 * 输出 p50/p99，供 M2-S5（PDP 收权后 check 走 cupolas_d RPC）与 S4
 * （epoch 主动失效）落地后对比回归。
 */

#include "gateway_pep_cache.h"
#include "gateway_biz_internal.h"

#include "airy_memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define BENCH_ITERS 5000
#define BENCH_WARMUP 1000
#define BENCH_HOT_KEYS 64

#ifndef _WIN32

static char g_sock_path[128];
static volatile int g_conns;      /* 假 PDP 收到的 RPC 次数 */
static volatile int g_allowed;    /* 假 PDP 裁定：1 allow / 0 deny */
static volatile uint64_t g_epoch; /* 假 PDP 权威 epoch */
static pthread_t g_srv;
static volatile int g_stop;

static void *fake_pdp(void *arg)
{
    (void)arg;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0)
        return NULL;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, g_sock_path);
    unlink(g_sock_path);
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 4) != 0) {
        close(srv);
        return NULL;
    }
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0)
            break;
        char buf[4096];
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            /* 仅统计实际 RPC 请求（就绪探测连接只 connect 不发数据） */
            g_conns++;
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"allowed\":%s,\"epoch\":%llu}}",
                     g_allowed ? "true" : "false", (unsigned long long)g_epoch);
            (void)send(c, resp, strlen(resp), 0);
        }
        close(c);
        if (g_stop)
            break;
    }
    close(srv);
    unlink(g_sock_path);
    return NULL;
}

static void fake_start(void)
{
    g_conns = 0;
    g_allowed = 1;
    g_epoch = 1;
    g_stop = 0;
    strcpy(g_sock_path, "/tmp/airy_pep_bench.sock");
    pthread_create(&g_srv, NULL, fake_pdp, NULL);
    /* 等待服务端就绪 */
    for (int i = 0; i < 50; i++) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, g_sock_path);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            close(fd);
            break;
        }
        if (fd >= 0)
            close(fd);
        usleep(20000);
    }
    /* 就绪探测的连接会触发一次 accept，不计入 RPC 计数 */
    g_conns = 0;
}

static void fake_stop(void)
{
    g_stop = 1;
    /* 触碰 socket 让 accept 返回（未监听时失败无害） */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, g_sock_path);
        (void)connect(fd, (struct sockaddr *)&a, sizeof(a));
        close(fd);
    }
    pthread_join(g_srv, NULL);
    unlink(g_sock_path);
}

#endif /* !_WIN32 */

static void out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int cmp_u64(const void *a, const void *b)
{
    return (*(const uint64_t *)a > *(const uint64_t *)b) -
           (*(const uint64_t *)a < *(const uint64_t *)b);
}

static void finish(uint64_t *times, size_t n, const char *name)
{
    qsort(times, n, sizeof(uint64_t), cmp_u64);
    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += times[i];
    out("  %-34s avg=%8.2fus min=%8.2f max=%8.2f p50=%8.2f p99=%8.2f\n", name,
        sum / (double)n, (double)times[0], (double)times[n - 1],
        (double)times[n / 2], (double)times[n * 99 / 100]);
}

static gateway_business_ctx_t make_ctx(void)
{
    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.cupolas_sock_path, g_sock_path);
    return ctx;
}

#ifndef _WIN32
static void bench_cold_path(const gateway_business_ctx_t *ctx, int iters)
{
    uint64_t *t = (uint64_t *)AIRY_MALLOC((size_t)iters * sizeof(uint64_t));
    if (!t)
        return;
    for (int i = 0; i < iters; i++) {
        char key[64];
        snprintf(key, sizeof(key), "tool.cold_%d", i);
        uint64_t s = now_us();
        gw_pep_check(ctx, "bench-agent", key, "execute");
        t[i] = now_us() - s;
    }
    finish(t, (size_t)iters, "cold path (RPC miss)");
    AIRY_FREE(t);
}

static void bench_hot_path(const gateway_business_ctx_t *ctx, int iters)
{
    uint64_t *t = (uint64_t *)AIRY_MALLOC((size_t)iters * sizeof(uint64_t));
    if (!t)
        return;
    for (int i = 0; i < iters; i++) {
        uint64_t s = now_us();
        gw_pep_check(ctx, "bench-agent", "tool.hot", "execute");
        t[i] = now_us() - s;
    }
    finish(t, (size_t)iters, "hot path (cache hit)");
    AIRY_FREE(t);
}

static void bench_epoch_invalidate(const gateway_business_ctx_t *ctx, int iters)
{
    /* 每 2000 次检查触发一次 epoch+1（模拟策略热更新），测失效后
     * 首个请求（整体重填）与稳定期请求的成本差。 */
    uint64_t *t = (uint64_t *)AIRY_MALLOC((size_t)iters * sizeof(uint64_t));
    if (!t)
        return;
    int rpc_before = g_conns;
    uint64_t invalidations = 0;
    for (int i = 0; i < iters; i++) {
        char key[64];
        snprintf(key, sizeof(key), "tool.epoch_%d", i % BENCH_HOT_KEYS);
        if (i % 2000 == 0) {
            g_epoch++;
            invalidations++;
        }
        uint64_t s = now_us();
        gw_pep_check(ctx, "bench-agent", key, "execute");
        t[i] = now_us() - s;
    }
    finish(t, (size_t)iters, "epoch-change load (S4 target)");
    out("  epoch invalidations=%llu extra_rpcs=%d\n",
        (unsigned long long)invalidations, g_conns - rpc_before);
    AIRY_FREE(t);
}

static void bench_mixed_load(const gateway_business_ctx_t *ctx, int iters)
{
    uint64_t *t = (uint64_t *)AIRY_MALLOC((size_t)iters * sizeof(uint64_t));
    if (!t)
        return;
    int base_conns = g_conns;
    for (int i = 0; i < iters; i++) {
        char key[64];
        if (i % 5 != 0) {
            /* 80% 热 key：重复命中 */
            snprintf(key, sizeof(key), "tool.mix_%d", i % BENCH_HOT_KEYS);
        } else {
            /* 20% 冷 key：新 key miss */
            snprintf(key, sizeof(key), "tool.coldmix_%d", i);
        }
        uint64_t s = now_us();
        gw_pep_check(ctx, "bench-agent", key, "execute");
        t[i] = now_us() - s;
    }
    finish(t, (size_t)iters, "mixed 80/20 load");
    int rpcs = g_conns - base_conns;
    double hit_rate = 100.0 * (double)(iters - rpcs) / (double)iters;
    out("  PEP hit_rate=%.2f%% (%d RPC / %d checks)\n", hit_rate, rpcs, iters);
    AIRY_FREE(t);
}
#endif

int main(int argc, char *argv[])
{
    int iters = BENCH_ITERS;
    if (argc > 1) {
        iters = atoi(argv[1]);
        if (iters < 100)
            iters = 100;
    }

    out("========================================\n");
    out("  AgentRT Gateway PEP End-to-End Baseline\n");
    out("  Iterations: %d (S1, 0.1.9 3.4)\n", iters);
    out("========================================\n");

    gw_pep_init();

#ifndef _WIN32
    fake_start();
    gateway_business_ctx_t ctx = make_ctx();

    /* 预热：命中后建立稳定缓存 */
    for (int i = 0; i < BENCH_WARMUP; i++)
        gw_pep_check(&ctx, "bench-agent", "tool.warm", "execute");
    gw_pep_clear();
    g_conns = 0;

    bench_cold_path(&ctx, iters);
    bench_hot_path(&ctx, iters);
    bench_epoch_invalidate(&ctx, iters);
    bench_mixed_load(&ctx, iters);

    fake_stop();
#else
    out("  skipped on Windows (unix socket)\n");
#endif

    gw_pep_clear();
    out("========================================\n");
    out("  Baseline complete (M2-S1)\n");
    out("========================================\n");
    return 0;
}
