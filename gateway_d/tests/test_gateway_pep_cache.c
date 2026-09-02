// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_pep_cache.c
 * @brief gateway PEP 裁定缓存单元测试（M2-S5，0.1.9 §3.2）。
 *
 * 覆盖：
 *   - fail-closed：空参数拒绝
 *   - 降级路径：PDP 不可达时回退本地 ACL（daemon_check_tool_permission）
 *   - 缓存命中：同一 (agent,tool,action) 第二次检查零 RPC
 *   - epoch 失效：PDP 返回新 epoch 后缓存整体失效并重新对齐
 *   - deny 裁定：裁定结果缓存（含 deny）
 *
 * 用假 PDP（unix socket 服务端线程）模拟 cupolas_d check_permission，
 * 不依赖真实 daemon（测试保持确定性）。
 */

#include "gateway_pep_cache.h"
#include "gateway_biz_internal.h"
#include "daemon_cupolas_bootstrap.h"
#include "daemon_security.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define TEST_PASS(name) printf("[PASS] %s\n", name)
#define TEST_FAIL(name, msg) printf("[FAIL] %s: %s\n", name, msg)

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond)                                      \
    do {                                                 \
        if (cond) {                                      \
            s_pass++;                                    \
        } else {                                         \
            s_fail++;                                    \
            TEST_FAIL(__func__, #cond);                  \
        }                                                \
    } while (0)

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
            buf[n] = '\0';
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
    strcpy(g_sock_path, "/tmp/airy_pep_fake.sock");
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
#endif

static void t_failclosed(void)
{
    /* 空参数：fail-closed 拒绝 */
    CHECK(gw_pep_check(NULL, NULL, "t", "x") != 0);
    CHECK(gw_pep_check(NULL, "a", NULL, "x") != 0);
    CHECK(gw_pep_check(NULL, "a", "t", NULL) != 0);
    CHECK(gw_pep_check(NULL, "", "t", "x") != 0);
    TEST_PASS("fail_closed");
}

static void t_epoch_parse(void)
{
    /* M2-S4：订阅帧解析（notify_d 广播 message 内层 JSON 未转义） */
    CHECK(gw_pep_epoch_parse(NULL) == 0);
    CHECK(gw_pep_epoch_parse("") == 0);
    /* 非目标 topic：即使含 epoch 键也不解析 */
    CHECK(gw_pep_epoch_parse("{\"topic\":\"airy.hall\",\"message\":\"{\"epoch\":7}\"}") == 0);
    /* 目标 topic 广播帧 → 权威 epoch */
    CHECK(gw_pep_epoch_parse("{\"event\":\"epoch_change\",\"topic\":\"airy.cupolas.epoch\","
                             "\"message\":\"{\"epoch\":3}\"}") == 3);
    /* 目标 topic 但缺 epoch 键 → 0 */
    CHECK(gw_pep_epoch_parse("{\"topic\":\"airy.cupolas.epoch\",\"message\":\"n/a\"}") == 0);
    /* 大位数（单调推进不截断） */
    CHECK(gw_pep_epoch_parse("{\"topic\":\"airy.cupolas.epoch\","
                             "\"message\":\"{\"epoch\":18446744073709551615}\"}") ==
          UINT64_MAX);
    TEST_PASS("epoch_parse");
}

static void t_fallback_acl(void)
{
    /* PDP 不可达（无 socket 路径）→ 降级本地 ACL */
    assert(daemon_cupolas_init_pep("pep_test") == AIRY_OK);
    daemon_security_add_acl_rule(GW_EXTERNAL_AGENT_ID, "fs_read", true);
    daemon_security_add_acl_rule(GW_EXTERNAL_AGENT_ID, "fs_write", false);

    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cupolas_sock_path[0] = '\0'; /* 无 PDP 端点 */

    CHECK(gw_pep_check(&ctx, GW_EXTERNAL_AGENT_ID, "fs_read", "execute") == 0);
    CHECK(gw_pep_check(&ctx, GW_EXTERNAL_AGENT_ID, "fs_write", "execute") != 0);
    CHECK(gw_pep_check(&ctx, GW_EXTERNAL_AGENT_ID, "shell_run", "execute") != 0);
    TEST_PASS("fallback_acl");
}

#ifndef _WIN32
static void t_cache_hit(void)
{
    gw_pep_clear();
    fake_start();
    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.cupolas_sock_path, g_sock_path);

    /* 第一次：miss → RPC（探测连接产生的计数为 baseline） */
    int base = g_conns;
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.a", "execute") == 0);
    CHECK(g_conns == base + 1);
    /* 第二次：命中缓存，零 RPC */
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.a", "execute") == 0);
    CHECK(g_conns == base + 1);
    fake_stop();
    TEST_PASS("cache_hit");
}

static void t_epoch_invalidate(void)
{
    gw_pep_clear();
    fake_start();
    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.cupolas_sock_path, g_sock_path);

    int base = g_conns;
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.a", "execute") == 0);
    int conns_after_first = g_conns;
    CHECK(conns_after_first == base + 1);

    /* 权威 epoch 变更（策略热更新生效）→ 缓存整体失效 */
    g_epoch = 2;
    CHECK(gw_pep_check(&ctx, "agent-2", "tool.b", "execute") == 0); /* 新 key miss */
    CHECK(g_conns == conns_after_first + 1);
    /* 旧 key 已失效 → 重新 RPC */
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.a", "execute") == 0);
    CHECK(g_conns == conns_after_first + 2);
    fake_stop();
    TEST_PASS("epoch_invalidate");
}

static void t_deny_cached(void)
{
    gw_pep_clear();
    fake_start();
    g_allowed = 0;
    gateway_business_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.cupolas_sock_path, g_sock_path);

    int base = g_conns;
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.deny", "execute") != 0);
    CHECK(g_conns == base + 1);
    /* deny 结果同样缓存 */
    CHECK(gw_pep_check(&ctx, "agent-1", "tool.deny", "execute") != 0);
    CHECK(g_conns == base + 1);
    fake_stop();
    TEST_PASS("deny_cached");
}
#endif

int main(void)
{
    gw_pep_init();
    t_failclosed();
    t_epoch_parse();
    t_fallback_acl();
#ifndef _WIN32
    t_cache_hit();
    t_epoch_invalidate();
    t_deny_cached();
#endif
    printf("gateway_pep_cache: %d pass, %d fail\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
