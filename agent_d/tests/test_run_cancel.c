// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_cancel.c
 * @brief 在途工具调用的取消契约（R11，white-box include）。
 *
 * 取消此前只挡住「下一轮」，工具调用本身仍阻塞在 RPC 上，用户要等到工具
 * 预算跑满才看到响应。本用例用一个真实 unix socket 模拟「收到请求后永不
 * 回执」的工具服务，断言：取消后客户端在 1 秒内返回 AIRY_ERR_CANCELED，
 * 且回执文本明确归因为用户取消。
 */

#include "../src/run/agent_run_loop.c"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "agent_d_internal.h"
agent_service_t *g_service = NULL;

static int g_fail;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *path;
    volatile int fd;
} srv_t;

/* 最简工具服务：接受一条连接后只读不回执，直到客户端关闭。
 * 不引入任何桩函数——它就是「工具长时间在跑」的真实对端形态。 */
static void *srv_run(void *ud)
{
    srv_t *s = (srv_t *)ud;
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
        return NULL;
    struct sockaddr_un a;
    AIRY_MEMSET(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", s->path);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 1) != 0) {
        close(lfd);
        return NULL;
    }
    s->fd = lfd;
    int c = accept(lfd, NULL, NULL);
    if (c >= 0) {
        char b[512];
        while (read(c, b, sizeof(b)) > 0)
            continue;
        close(c);
    }
    return NULL;
}

typedef struct {
    airy_cancel_token_t *tok;
    int delay_ms;
} canceller_t;

/* 模拟用户在工具在途时按下取消。 */
static void *cancel_run(void *ud)
{
    canceller_t *c = (canceller_t *)ud;
    usleep((useconds_t)(c->delay_ms * 1000));
    airy_cancel_token_cancel(c->tok);
    return NULL;
}

/* R11 主用例：取消必须打断等待，而不是等工具预算跑满。
 * 工具预算为 AGENT_RUN_TOOL_TIMEOUT_MS（数十秒），3s 上限足以把
 * 「取消真的生效」与「碰巧超时」区分开。 */
static void test_inflight_cancel(void)
{
    char tmpl[] = "/tmp/airy-cancel-XXXXXX";
    char *home = mkdtemp(tmpl);
    CHECK(home != NULL, "temp home created");
    if (!home)
        return;
    setenv("AIRY_HOME", home, 1);
    char run_dir[AIRY_PATH_MAX];
    snprintf(run_dir, sizeof(run_dir), "%s/run", home);
    mkdir(run_dir, 0700);

    airy_cancel_token_t tok;
    CHECK(airy_cancel_token_init(&tok) == 0, "token init");
    CHECK(!airy_cancel_token_is_canceled(&tok), "token starts clear");

    srv_t srv = {airy_runtime_dir_socket("tool.sock"), -1};
    pthread_t st;
    CHECK(pthread_create(&st, NULL, srv_run, &srv) == 0, "tool server thread");
    for (int i = 0; i < 200 && srv.fd < 0; i++)
        usleep(5000);
    CHECK(srv.fd >= 0, "tool server listening");

    canceller_t cc = {&tok, 100};
    pthread_t ct;
    CHECK(pthread_create(&ct, NULL, cancel_run, &cc) == 0, "canceller thread");

    char *out = NULL;
    int code = 0;
    uint64_t t0 = airy_time_ms();
    int rc =
        run_execute_tool("fs_read", "{\"path\":\"/tmp\"}", &out, &code, false, &tok, "coding_v1");
    uint64_t ms = airy_time_ms() - t0;

    /* 执行入口以 -1 记失败、把归因放进回执：取消与超时都以失败返回，
     * 区分二者的是回执文案与耗时，故必须同时断言后两项。 */
    CHECK(rc != 0, "in-flight cancel fails the call");
    CHECK(out && strstr(out, "canceled by user") != NULL, "receipt names user cancel");
    CHECK(out && strstr(out, "timed out") == NULL, "receipt is not a timeout");
    CHECK(ms < 3000, "cancel interrupts the wait rather than waiting for tool budget");

    AIRY_FREE(out);
    pthread_join(ct, NULL);
    pthread_join(st, NULL);
    if (srv.fd >= 0)
        close(srv.fd);
    unlink(srv.path);
    airy_cancel_token_destroy(&tok);
}

int main(void)
{
    test_inflight_cancel();
    if (g_fail == 0) {
        printf("test_run_cancel: all checks passed\n");
        return 0;
    }
    printf("test_run_cancel: %d check(s) failed\n", g_fail);
    return 1;
}
