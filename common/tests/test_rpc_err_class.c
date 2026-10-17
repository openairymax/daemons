// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_rpc_err_class.c
 * @brief L1 传输层错误分类回归测试（0.1.19 §3.7.2 N1）。
 *
 * 覆盖三层责任模型的中间层：对端已应答时 JSON-RPC 错误必须按其码保义，
 * 绝不上溢为 transport；链路自身失败才归 transport。全部用例经真实
 * AF_UNIX listener 往返（非桩），断言返回码与其 class 的对应关系。
 */

#include "daemon_rpc_client.h"
#include "err_class.h"
#include "error_codes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char path[128];
} mock_srv_t;

static void *srv_run(void *arg)
{
    mock_srv_t *m = (mock_srv_t *)arg;
    int c = accept(m->fd, NULL, NULL);
    if (c < 0)
        return NULL;
    char req[4096];
    (void)recv(c, req, sizeof(req), 0);
    if (m->hold_ms > 0) {
        struct timespec ts = {m->hold_ms / 1000, (m->hold_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
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

/* 建立一次性 mock daemon；成功返回 0，线程句柄写入 tid。 */
static int srv_up(mock_srv_t *m, const char *tag, pthread_t *tid)
{
    struct sockaddr_un a;

    snprintf(m->path, sizeof(m->path), "/tmp/airy_n1_%s_%d.sock", tag, (int)getpid());
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

/* 跑一次真实往返，返回 daemon_rpc_call 的错误码（0 表示成功）。 */
static int rpc_case(const char *tag, const char *reply, long hold_ms, uint32_t tmo)
{
    mock_srv_t m;
    pthread_t tid;
    char *out = NULL;
    int rc;

    memset(&m, 0, sizeof(m));
    m.reply = reply;
    m.hold_ms = hold_ms;
    assert(srv_up(&m, tag, &tid) == 0);
    rc = daemon_rpc_call(m.path, "probe", "{}", &out, tmo);
    pthread_join(tid, NULL);
    close(m.fd);
    unlink(m.path);
    if (rc == AIRY_SUCCESS)
        assert(out != NULL && out[0] != '\0');
    else
        assert(out == NULL);
    free(out);
    return rc;
}

static void test_business(void)
{
    int rc;

    printf("  test_business...\n");
    /* 对端应答 -32602：参数错误属 business，不得折叠为 transport */
    rc = rpc_case("badparam", "{\"jsonrpc\":\"2.0\",\"id\":1,"
                              "\"error\":{\"code\":-32602,\"message\":\"bad params\"}}",
                  0, 2000);
    assert(rc == AIRY_ERR_INVALID_PARAM);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_BUSINESS);

    /* -32601：方法不存在同样属 business */
    rc = rpc_case("nomethod", "{\"jsonrpc\":\"2.0\",\"id\":1,"
                              "\"error\":{\"code\":-32601,\"message\":\"no method\"}}",
                  0, 2000);
    assert(rc == AIRY_ERR_NOT_SUPPORTED);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_BUSINESS);

    /* -32603：服务端内部错误按通用业务失败承载 */
    rc = rpc_case("srvfail", "{\"jsonrpc\":\"2.0\",\"id\":1,"
                             "\"error\":{\"code\":-32603,\"message\":\"boom\"}}",
                  0, 2000);
    assert(rc == AIRY_ERR_GENERIC_FAIL);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_BUSINESS);
    printf("    PASSED\n");
}

static void test_success(void)
{
    int rc;

    printf("  test_success...\n");
    rc = rpc_case("ok", "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"success\":1}}", 0, 2000);
    assert(rc == AIRY_SUCCESS);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_OK);
    printf("    PASSED\n");
}

static void test_transport(void)
{
    int rc;

    printf("  test_transport...\n");
    /* 截断响应 + 对端关闭：链路可达但协议不完整 */
    rc = rpc_case("trunc", "{\"jsonrpc\":\"2.0\",\"id\":1,\"resu", 0, 2000);
    assert(rc == AIRY_ERR_PROTOCOL);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_TRANSPORT);

    /* 对端沉默超过 deadline：超时属 transport */
    rc = rpc_case("silent", NULL, 600, 150);
    assert(rc == AIRY_ERR_TIMEOUT);
    assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_TRANSPORT);

    /* 无监听者：连接失败属 transport（原误分类为 NOT_FOUND/business） */
    {
        char path[128];
        char *out = NULL;
        snprintf(path, sizeof(path), "/tmp/airy_n1_absent_%d.sock", (int)getpid());
        unlink(path);
        rc = daemon_rpc_call(path, "probe", "{}", &out, 500);
        assert(rc == AIRY_ERR_SYS_SOCKET);
        assert(airy_err_class_of(rc) == AIRY_ERR_CLASS_TRANSPORT);
        assert(out == NULL);
    }
    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  rpc_err_class Unit Tests\n");
    printf("=========================================\n");

    signal(SIGPIPE, SIG_IGN);
    test_business();
    test_success();
    test_transport();

    printf("\nAll rpc_err_class tests PASSED\n");
    return 0;
}

#else /* _WIN32 */

int main(void)
{
    printf("rpc_err_class: POSIX-only (AF_UNIX mock daemon), skipped\n");
    return 0;
}

#endif
