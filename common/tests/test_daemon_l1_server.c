// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_l1_server.c
 * @brief WS-8 stage 3 (8.3.1): daemon L1 server mount unit tests
 *
 * 测试场景：
 *   1. transport 开关三级解析：默认关 / 全局开 / ns 覆盖开 / ns 覆盖关 /
 *      未知值 fail-closed / ns=NULL 仅看全局
 *   2. start 参数校验：NULL 名 / 空名 / NULL handler / 超长名
 *   3. 进程内回环：connect + send 同步触达 handler（code/data/size/msg_id）
 *   4. 重名拒绝（EEXIST 语义）与 stop 后重挂
 *   5. stop 后新 connect 得 ENOENT、残存 client send 得 ECANCELED
 *      （依赖 atoms 284e6b6 的 binder close 排空语义）
 *
 * 客户端直接使用 corekern L1 API（airy_ipc_connect/send）模拟对端；
 * corekern 头仅在本测试 TU 使用，不违反 svc_common 的宏隔离边界。
 */

#include "daemon_l1_server.h"

#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name)           \
    do {                           \
        printf("  %s...\n", name); \
    } while (0)

#define TEST_ASSERT(cond, msg)                                 \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("    FAIL: %s (line %d)\n", msg, __LINE__); \
            g_tests_failed++;                                  \
            return;                                            \
        }                                                      \
    } while (0)

#define TEST_END()              \
    do {                        \
        printf("    PASSED\n"); \
        g_tests_passed++;       \
    } while (0)

/* ---- transport 开关解析 ---- */

static void test_transport_default_off(void)
{
    TEST_BEGIN("test_transport_default_off");

    unsetenv("AIRY_IPC_TRANSPORT");
    unsetenv("AIRY_SCHED_IPC_TRANSPORT");

    TEST_ASSERT(!daemon_l1_transport_enabled("SCHED"), "no env -> off");
    TEST_ASSERT(!daemon_l1_transport_enabled(NULL), "no env, ns=NULL -> off");

    TEST_END();
}

static void test_transport_global_switch(void)
{
    TEST_BEGIN("test_transport_global_switch");

    unsetenv("AIRY_SCHED_IPC_TRANSPORT");
    setenv("AIRY_IPC_TRANSPORT", "corekern", 1);
    TEST_ASSERT(daemon_l1_transport_enabled("SCHED"), "global corekern -> on");
    TEST_ASSERT(daemon_l1_transport_enabled(NULL), "global corekern, ns=NULL -> on");

    setenv("AIRY_IPC_TRANSPORT", "jsonrpc", 1);
    TEST_ASSERT(!daemon_l1_transport_enabled("SCHED"), "global jsonrpc -> off");

    setenv("AIRY_IPC_TRANSPORT", "bogus", 1);
    TEST_ASSERT(!daemon_l1_transport_enabled("SCHED"), "unknown value -> fail-closed off");

    unsetenv("AIRY_IPC_TRANSPORT");

    TEST_END();
}

static void test_transport_ns_override(void)
{
    TEST_BEGIN("test_transport_ns_override");

    setenv("AIRY_IPC_TRANSPORT", "jsonrpc", 1);
    setenv("AIRY_SCHED_IPC_TRANSPORT", "corekern", 1);
    TEST_ASSERT(daemon_l1_transport_enabled("SCHED"), "ns override wins over global off");

    setenv("AIRY_SCHED_IPC_TRANSPORT", "jsonrpc", 1);
    setenv("AIRY_IPC_TRANSPORT", "corekern", 1);
    TEST_ASSERT(!daemon_l1_transport_enabled("SCHED"), "ns override wins over global on");

    setenv("AIRY_SCHED_IPC_TRANSPORT", "weird", 1);
    TEST_ASSERT(!daemon_l1_transport_enabled("SCHED"), "ns unknown value -> fail-closed off");

    unsetenv("AIRY_IPC_TRANSPORT");
    unsetenv("AIRY_SCHED_IPC_TRANSPORT");

    TEST_END();
}

/* ---- start 参数校验 ---- */

static int noop_handler(uint32_t code, const void *data, size_t size, uint64_t msg_id,
                        void *userdata)
{
    (void)code;
    (void)data;
    (void)size;
    (void)msg_id;
    (void)userdata;
    return 0;
}

static void test_start_param_validation(void)
{
    TEST_BEGIN("test_start_param_validation");

    TEST_ASSERT(daemon_l1_server_start(NULL, noop_handler, NULL) == NULL, "NULL name rejected");
    TEST_ASSERT(daemon_l1_server_start("", noop_handler, NULL) == NULL, "empty name rejected");
    TEST_ASSERT(daemon_l1_server_start("l1t.valid", NULL, NULL) == NULL, "NULL handler rejected");

    /* DAEMON_L1_NAME_MAX = 64：64 字节名（不含终止符）必须被拒 */
    char long_name[65];
    memset(long_name, 'x', sizeof(long_name));
    long_name[64] = '\0';
    TEST_ASSERT(strlen(long_name) == 64, "boundary name prepared (64 bytes)");
    TEST_ASSERT(daemon_l1_server_start(long_name, noop_handler, NULL) == NULL,
                "64-byte name rejected (corekern MAX_CHANNEL_NAME)");

    daemon_l1_server_stop(NULL); /* NULL 安全：不得崩溃 */

    TEST_END();
}

/* ---- 进程内回环 ---- */

struct loopback_ctx {
    int hits;
    uint32_t last_code;
    uint64_t last_msg_id;
    size_t last_size;
    char last_data[32];
    void *expect_userdata;
};

static int loopback_handler(uint32_t code, const void *data, size_t size, uint64_t msg_id,
                            void *userdata)
{
    struct loopback_ctx *ctx = (struct loopback_ctx *)userdata;
    ctx->hits++;
    ctx->last_code = code;
    ctx->last_msg_id = msg_id;
    ctx->last_size = size;
    if (data && size < sizeof(ctx->last_data)) {
        memcpy(ctx->last_data, data, size);
    }
    return 0;
}

static void test_loopback_send_reaches_handler(void)
{
    TEST_BEGIN("test_loopback_send_reaches_handler");

    unsetenv("AIRY_IPC_TRANSPORT");

    struct loopback_ctx ctx = {.expect_userdata = NULL};
    daemon_l1_server_t *svc = daemon_l1_server_start("l1t.loopback", loopback_handler, &ctx);
    TEST_ASSERT(svc != NULL, "server mount succeeded");
    TEST_ASSERT(strcmp(daemon_l1_server_channel_name(svc), "l1t.loopback") == 0,
                "channel_name getter round-trips");

    airy_ipc_channel_t *client = NULL;
    TEST_ASSERT(airy_ipc_connect("l1t.loopback", &client) == AIRY_SUCCESS, "client connected");

    static char payload[] = "l1t-ping";
    airy_kernel_ipc_message_t msg = {.code = 42,
                                     .data = payload,
                                     .size = sizeof(payload),
                                     .fd = -1,
                                     .msg_id = 77};
    /* 同步事务语义：send 返回时 handler 已在发送方线程执行完毕。
     * msg_id 为 binder 分配的全局事务 id，caller 值 77 会被覆盖。 */
    TEST_ASSERT(airy_ipc_send(client, &msg) == AIRY_SUCCESS, "send succeeded");

    TEST_ASSERT(ctx.hits == 1, "handler invoked exactly once");
    TEST_ASSERT(ctx.last_code == 42, "code propagated");
    TEST_ASSERT(ctx.last_msg_id != 77, "msg_id reassigned by binder (not caller value)");
    TEST_ASSERT(ctx.last_size == sizeof(payload), "size propagated");
    TEST_ASSERT(strcmp(ctx.last_data, "l1t-ping") == 0, "payload bytes propagated");

    TEST_ASSERT(airy_ipc_close(client) == AIRY_SUCCESS, "client closed");
    daemon_l1_server_stop(svc);
    airy_ipc_cleanup();

    TEST_END();
}

/* ---- 重名拒绝与 stop 后重挂 ---- */

static void test_duplicate_name_and_remount(void)
{
    TEST_BEGIN("test_duplicate_name_and_remount");

    struct loopback_ctx ctx = {0};
    daemon_l1_server_t *first = daemon_l1_server_start("l1t.dup", loopback_handler, &ctx);
    TEST_ASSERT(first != NULL, "first mount succeeded");

    daemon_l1_server_t *second = daemon_l1_server_start("l1t.dup", loopback_handler, &ctx);
    TEST_ASSERT(second == NULL, "duplicate name rejected (EEXIST semantics)");

    daemon_l1_server_stop(first);

    airy_ipc_channel_t *probe = NULL;
    TEST_ASSERT(airy_ipc_connect("l1t.dup", &probe) == AIRY_ENOENT,
                "stopped channel unlinked from registry");

    daemon_l1_server_t *remount = daemon_l1_server_start("l1t.dup", loopback_handler, &ctx);
    TEST_ASSERT(remount != NULL, "remount after stop succeeded");

    daemon_l1_server_stop(remount);
    airy_ipc_cleanup();

    TEST_END();
}

/* ---- stop 后 connect/send 语义（依赖 binder close 排空） ---- */

static void test_stop_rejects_connect_and_send(void)
{
    TEST_BEGIN("test_stop_rejects_connect_and_send");

    struct loopback_ctx ctx = {0};
    daemon_l1_server_t *svc = daemon_l1_server_start("l1t.stop", loopback_handler, &ctx);
    TEST_ASSERT(svc != NULL, "server mount succeeded");

    airy_ipc_channel_t *client = NULL;
    TEST_ASSERT(airy_ipc_connect("l1t.stop", &client) == AIRY_SUCCESS, "client connected");

    daemon_l1_server_stop(svc);

    airy_ipc_channel_t *late = NULL;
    TEST_ASSERT(airy_ipc_connect("l1t.stop", &late) == AIRY_ENOENT,
                "connect after stop -> ENOENT");

    static char payload[] = "late";
    airy_kernel_ipc_message_t msg = {.code = 1,
                                     .data = payload,
                                     .size = sizeof(payload),
                                     .fd = -1,
                                     .msg_id = 1};
    TEST_ASSERT(airy_ipc_send(client, &msg) == AIRY_ECANCELED,
                "residual client send after stop -> ECANCELED (no UAF)");
    TEST_ASSERT(ctx.hits == 0, "handler never invoked after stop");

    TEST_ASSERT(airy_ipc_close(client) == AIRY_SUCCESS, "residual client closed");
    airy_ipc_cleanup();

    TEST_END();
}

int main(void)
{
    printf("========================================\n");
    printf("  daemon_l1_server unit tests (8.3.1)\n");
    printf("========================================\n");

    test_transport_default_off();
    test_transport_global_switch();
    test_transport_ns_override();
    test_start_param_validation();
    test_loopback_send_reaches_handler();
    test_duplicate_name_and_remount();
    test_stop_rejects_connect_and_send();

    printf("\n========================================\n");
    printf("  daemon_l1_server 测试结果汇总\n");
    printf("========================================\n");
    printf("  通过:   %d\n", g_tests_passed);
    printf("  失败:   %d\n", g_tests_failed);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
