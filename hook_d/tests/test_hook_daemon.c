// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_hook_daemon.c
 * @brief hook_d 守护进程 JSON-RPC 冒烟测试（POSIX）。
 *
 * 启动真实 hook_d 二进制，通过 Unix socket 验证 health/ping/status/
 * register/trigger/stats/unregister 等 RPC 方法，覆盖 daemon main.c 的
 * 完整接线（含 L2 标准方法）。Windows 下跳过。
 */

#ifndef _WIN32

#include "daemon_rpc_client.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TEST_BIN_DIR
#define TEST_BIN_DIR ""
#endif

#define HOOK_SOCKET_NAME "hook.sock"
#define HOOK_TEST_WAIT_MS 10000

static char g_runtime_dir[512];
static char g_socket_path[576];
static char g_log_path[576];
static pid_t g_child = -1;

static int wait_for_socket(int timeout_ms)
{
    struct timespec ts = {0, 50 * 1000 * 1000}; /* 50ms */
    int waited = 0;
    while (waited < timeout_ms) {
        if (access(g_socket_path, F_OK) == 0)
            return 0;
        nanosleep(&ts, NULL);
        waited += 50;
    }
    return -1;
}

static int spawn_hook_d(void)
{
    char bin[512];
    snprintf(bin, sizeof(bin), "%s/hook_d", TEST_BIN_DIR);

    /* 运行时根由 AIRY_HOME 决定（g_run_dir = $AIRY_HOME/run）；daemon
     * 不会自行调用 airy_paths_init，需预创建 run 目录 */
    char run_dir[576];
    snprintf(run_dir, sizeof(run_dir), "%s/run", g_runtime_dir);
    mkdir(g_runtime_dir, 0755);
    mkdir(run_dir, 0755);
    setenv("AIRY_HOME", g_runtime_dir, 1);
    unsetenv("AIRY_RUNTIME_DIR");
    snprintf(g_socket_path, sizeof(g_socket_path), "%s/%s", run_dir, HOOK_SOCKET_NAME);
    snprintf(g_log_path, sizeof(g_log_path), "%s/hook_d.log", g_runtime_dir);

    g_child = fork();
    if (g_child < 0) {
        perror("fork");
        return -1;
    }
    if (g_child == 0) {
        FILE *log = freopen(g_log_path, "w", stdout);
        if (!log) {
            _exit(127);
        }
        dup2(fileno(log), STDERR_FILENO);
        execl(bin, bin, NULL);
        fprintf(stderr, "exec hook_d failed: %s\n", bin);
        _exit(127);
    }

    if (wait_for_socket(HOOK_TEST_WAIT_MS) != 0) {
        printf("    FAIL: hook.sock not created within %d ms\n", HOOK_TEST_WAIT_MS);
        kill(g_child, SIGKILL);
        waitpid(g_child, NULL, 0);
        g_child = -1;
        return -1;
    }
    return 0;
}

static void stop_hook_d(void)
{
    if (g_child > 0) {
        /* 先尝试优雅 shutdown，再兜底 SIGTERM */
        char *res = NULL;
        (void)daemon_rpc_call(g_socket_path, "shutdown", NULL, &res, 5000);
        AIRY_FREE(res);

        int status = 0;
        for (int i = 0; i < 50; i++) {
            if (waitpid(g_child, &status, WNOHANG) == g_child) {
                g_child = -1;
                break;
            }
            usleep(100 * 1000);
        }
        if (g_child > 0) {
            kill(g_child, SIGTERM);
            waitpid(g_child, NULL, 0);
            g_child = -1;
        }
    }
    unlink(g_socket_path);
}

static void test_health(void)
{
    printf("  test_health...\n");
    char *res = NULL;
    int ret = daemon_rpc_call(g_socket_path, "health", NULL, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"healthy\"") != NULL);
    assert(strstr(res, "\"hook_count\"") != NULL);
    AIRY_FREE(res);
    printf("    PASSED\n");
}

static void test_ping(void)
{
    printf("  test_ping...\n");
    char *res = NULL;
    int ret = daemon_rpc_call(g_socket_path, "ping", NULL, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"status\"") != NULL);
    assert(strstr(res, "\"uptime_sec\"") != NULL);
    AIRY_FREE(res);
    printf("    PASSED\n");
}

static void test_status(void)
{
    printf("  test_status...\n");
    char *res = NULL;
    int ret = daemon_rpc_call(g_socket_path, "status", NULL, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"service\"") != NULL);
    assert(strstr(res, "hook_d") != NULL);
    assert(strstr(res, "\"by_type\"") != NULL);
    AIRY_FREE(res);
    printf("    PASSED\n");
}

static void test_list(void)
{
    printf("  test_list...\n");
    char *res = NULL;
    int ret = daemon_rpc_call(g_socket_path, "list", NULL, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"hooks\"") != NULL);
    assert(strstr(res, "\"count\"") != NULL);
    AIRY_FREE(res);
    printf("    PASSED\n");
}

static void test_register_trigger_unregister(void)
{
    printf("  test_register_trigger_unregister...\n");

    /* register */
    const char *reg_params = "{\"name\":\"smoke_test_hook\",\"type\":\"pre_exec\","
                             "\"impl\":\"shell\",\"script_path\":\"/bin/true\","
                             "\"priority\":10,\"enabled\":true}";
    char *res = NULL;
    int ret = daemon_rpc_call(g_socket_path, "register", reg_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"status\"") != NULL);
    assert(strstr(res, "registered") != NULL);
    AIRY_FREE(res);

    /* trigger: 决策字段必须存在且为合法决策名 */
    const char *trig_params = "{\"type\":\"pre_exec\",\"operation\":\"smoke\","
                              "\"input\":\"hello\"}";
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "trigger", trig_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"decision_name\"") != NULL);
    AIRY_FREE(res);

    /* stats: 已注册 hook 可查询 */
    const char *stats_params = "{\"name\":\"smoke_test_hook\"}";
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "stats", stats_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "\"invoke_count\"") != NULL);
    AIRY_FREE(res);

    /* unregister */
    const char *unreg_params = "{\"name\":\"smoke_test_hook\"}";
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "unregister", unreg_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL);
    assert(strstr(res, "unregistered") != NULL);
    AIRY_FREE(res);

    /* P1-5：session_start 会话级 hook 的 register + trigger + unregister 往返 */
    const char *sess_reg = "{\"name\":\"smoke_session_start\",\"type\":\"session_start\","
                           "\"impl\":\"shell\",\"script_path\":\"/bin/true\","
                           "\"priority\":10,\"enabled\":true}";
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "register", sess_reg, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL && strstr(res, "registered") != NULL);
    AIRY_FREE(res);
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "trigger",
                          "{\"type\":\"session_start\",\"operation\":\"smoke_session\","
                          "\"input\":\"hello\"}",
                          &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL && strstr(res, "\"decision_name\"") != NULL);
    AIRY_FREE(res);
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "unregister", "{\"name\":\"smoke_session_start\"}",
                          &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    assert(res != NULL && strstr(res, "unregistered") != NULL);
    AIRY_FREE(res);

    /* 重复注册同名 hook 应失败（幂等校验） */
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "register", reg_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    AIRY_FREE(res);
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "register", reg_params, &res, 5000);
    /* 第二次注册应报错（名称已存在） */
    assert(res == NULL);
    AIRY_FREE(res);

    /* 清理 */
    res = NULL;
    ret = daemon_rpc_call(g_socket_path, "unregister", unreg_params, &res, 5000);
    assert(ret == AIRY_SUCCESS);
    (void)ret;
    AIRY_FREE(res);

    printf("    PASSED\n");
}

int main(void)
{
    printf("hook_d daemon JSON-RPC smoke tests\n");

    if (!TEST_BIN_DIR[0]) {
        printf("    SKIP: TEST_BIN_DIR not set\n");
        return 0;
    }

    snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/tmp/agentrt-hook-test-%ld",
             (long)getpid());

    if (spawn_hook_d() != 0) {
        printf("FAILED: cannot spawn hook_d from %s\n", TEST_BIN_DIR);
        return 1;
    }

    test_health();
    test_ping();
    test_status();
    test_list();
    test_register_trigger_unregister();

    stop_hook_d();

    printf("ALL PASSED\n");
    return 0;
}

#else /* _WIN32 */

int main(void)
{
    printf("hook_d daemon smoke tests: SKIP (POSIX only)\n");
    return 0;
}

#endif /* _WIN32 */
