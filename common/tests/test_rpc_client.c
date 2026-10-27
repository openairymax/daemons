// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_rpc_client.c
 * @brief L1 传输层超时分档与可达性负缓存回归测试（0.1.20 D2-1/D2-4）。
 *
 * D2-1：调用方传 0 时按方法查表，显式非 0 超时永远优先。
 * D2-4：只有「曾经连通过」的目标失败后才进入退避；首次不可达不缓存，
 * 以避免把 daemon 启动竞态放大为假不可达。全部用例经真实 AF_UNIX
 * listener 往返（非桩），以 accept 计数作为「是否真的发起连接」的证据。
 */

#include "daemon_rpc_client.h"
#include "error_codes.h"
#include "platform_misc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK_REPLY "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":1}}"

/* ---- 分档表（平台无关，两平台均验证） ---- */

static void test_tmo_table(void)
{
    printf("  test_tmo_table...\n");
    assert(daemon_rpc_timeout_for("complete_stream") == 300000u);
    assert(daemon_rpc_timeout_for("complete") == 180000u);
    assert(daemon_rpc_timeout_for("invoke") == 180000u);
    assert(daemon_rpc_timeout_for("think") == 120000u);
    assert(daemon_rpc_timeout_for("execute_tool") == 90000u);
    /* 表外方法、空串、NULL 一律回落默认 30s */
    assert(daemon_rpc_timeout_for("write") == 30000u);
    assert(daemon_rpc_timeout_for("") == 30000u);
    assert(daemon_rpc_timeout_for(NULL) == 30000u);
    printf("    PASSED\n");
}

#ifndef _WIN32

#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    const char *reply;
    long hold_ms;
    int conns;
    char path[128];
} mock_srv_t;

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *srv_run(void *arg)
{
    mock_srv_t *m = (mock_srv_t *)arg;
    int c = accept(m->fd, NULL, NULL);
    if (c < 0)
        return NULL;
    m->conns++;
    char req[4096];
    (void)recv(c, req, sizeof(req), 0);
    if (m->hold_ms > 0)
        sleep_ms(m->hold_ms);
    if (m->reply) {
        size_t n = strlen(m->reply);
        size_t off = 0;
        while (off < n) {
            ssize_t w = send(c, m->reply + off, n - off, 0);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
    }
    close(c);
    return NULL;
}

/* 在指定路径建立一次性 mock daemon；成功返回 0，线程句柄写入 tid。 */
static int srv_at(mock_srv_t *m, const char *path, pthread_t *tid)
{
    struct sockaddr_un a;

    snprintf(m->path, sizeof(m->path), "%s", path);
    unlink(m->path);
    m->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m->fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", m->path);
    if (bind(m->fd, (const struct sockaddr *)&a, sizeof(a)) != 0 || listen(m->fd, 1) != 0) {
        close(m->fd);
        return -1;
    }
    return pthread_create(tid, NULL, srv_run, m);
}

static void srv_stop(mock_srv_t *m, pthread_t tid)
{
    close(m->fd);
    pthread_join(tid, NULL);
    unlink(m->path);
}

/* D2-1：显式超时优先于方法分档（"complete" 档位 180s，显式 150ms 必须胜出）。 */
static void test_tmo_explicit(void)
{
    mock_srv_t m;
    pthread_t tid;
    char *out = NULL;
    uint64_t t0, t1;
    int rc;

    printf("  test_tmo_explicit...\n");
    memset(&m, 0, sizeof(m));
    m.reply = OK_REPLY;
    m.hold_ms = 600;
    {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/airy_tmo_%d.sock", (int)getpid());
        assert(srv_at(&m, path, &tid) == 0);
        t0 = airy_time_ms();
        rc = daemon_rpc_call(m.path, "complete", "{}", &out, 150);
        t1 = airy_time_ms();
    }
    assert(rc == AIRY_ERR_TIMEOUT);
    assert(out == NULL);
    assert(t1 - t0 < 500);
    srv_stop(&m, tid);
    printf("    PASSED\n");
}

/* D2-4a：首次不可达不进入退避——listener 出现后必须立即连通。 */
static void test_neg_first(void)
{
    mock_srv_t m;
    pthread_t tid;
    char *out = NULL;
    char path[128];
    int rc;

    printf("  test_neg_first...\n");
    snprintf(path, sizeof(path), "/tmp/airy_neg_first_%d.sock", (int)getpid());
    unlink(path);
    rc = daemon_rpc_call(path, "probe", "{}", &out, 500);
    assert(rc == AIRY_ERR_SYS_SOCKET);
    assert(out == NULL);

    memset(&m, 0, sizeof(m));
    m.reply = OK_REPLY;
    assert(srv_at(&m, path, &tid) == 0);
    rc = daemon_rpc_call(path, "probe", "{}", &out, 2000);
    assert(rc == AIRY_SUCCESS);
    assert(out != NULL);
    free(out);
    srv_stop(&m, tid);
    assert(m.conns == 1);
    printf("    PASSED\n");
}

/* D2-4b：曾经连通 → 失败进入退避（不再 connect）→ 到期后自动恢复。 */
static void test_neg_backoff(void)
{
    mock_srv_t m;
    pthread_t tid;
    char *out = NULL;
    char path[128];
    uint64_t t0, t1;
    int rc;

    printf("  test_neg_backoff...\n");
    snprintf(path, sizeof(path), "/tmp/airy_neg_seen_%d.sock", (int)getpid());
    unlink(path);

    /* 1) 成功一次，登记 seen=1 */
    memset(&m, 0, sizeof(m));
    m.reply = OK_REPLY;
    assert(srv_at(&m, path, &tid) == 0);
    rc = daemon_rpc_call(path, "probe", "{}", &out, 2000);
    assert(rc == AIRY_SUCCESS);
    free(out);
    out = NULL;
    srv_stop(&m, tid);

    /* 2) 目标消失 → 失败，写入 1s 退避 */
    rc = daemon_rpc_call(path, "probe", "{}", &out, 500);
    assert(rc == AIRY_ERR_SYS_SOCKET);
    assert(out == NULL);

    /* 3) 立即重连：命中退避，accept 计数必须为 0（证明未发起连接） */
    memset(&m, 0, sizeof(m));
    m.reply = OK_REPLY;
    assert(srv_at(&m, path, &tid) == 0);
    t0 = airy_time_ms();
    rc = daemon_rpc_call(path, "probe", "{}", &out, 2000);
    t1 = airy_time_ms();
    assert(rc == AIRY_ERR_SYS_SOCKET);
    assert(out == NULL);
    assert(t1 - t0 < 1000);
    srv_stop(&m, tid);
    assert(m.conns == 0);

    /* 4) 退避到期后恢复：真实可达性回来，连接数回到 1 */
    memset(&m, 0, sizeof(m));
    m.reply = OK_REPLY;
    assert(srv_at(&m, path, &tid) == 0);
    sleep_ms(1200);
    rc = daemon_rpc_call(path, "probe", "{}", &out, 2000);
    assert(rc == AIRY_SUCCESS);
    assert(out != NULL);
    free(out);
    srv_stop(&m, tid);
    assert(m.conns == 1);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  rpc_client Unit Tests (0.1.20 D2-1/D2-4)\n");
    printf("=========================================\n");

    signal(SIGPIPE, SIG_IGN);
    test_tmo_table();
    test_tmo_explicit();
    test_neg_first();
    test_neg_backoff();

    printf("\nAll rpc_client tests PASSED\n");
    return 0;
}

#else /* _WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  rpc_client Unit Tests (0.1.20 D2-1/D2-4)\n");
    printf("=========================================\n");

    /* 负缓存用例依赖 AF_UNIX mock daemon，Windows 走 TCP 回环另行覆盖 */
    test_tmo_table();

    printf("\nAll rpc_client tests PASSED\n");
    return 0;
}

#endif
