// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file test_memrpc.c
 * @brief memrpc 记忆提供商端到端测试（fork mem_d 真实 RPC 闭环）。
 *
 * mkdtemp 隔离 AIRY_HOME 后拉起真实 mem_d 二进制（项目内首个
 * daemon 子进程测试），经 IPC ops 注入链驱动 memrpc provider 全
 * vtable，覆盖 wire 协议与 provider 适配层语义；daemon 生命周期
 * 由测试全程托管（SIGTERM + waitpid）。Windows 无 fork，跳过。
 */

#include "provider.h"
#include "airy_ipc_ops.h"
#include "airy_memory.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int main(void)
{
    printf("  test_memrpc: skipped on Windows (fork-based E2E)\n");
    return 0;
}

#else /* POSIX */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_home[512];
static pid_t g_daemon = -1;

static int t_wait_socket(const char *path)
{
    for (int i = 0; i < 100; i++) {
        if (access(path, F_OK) == 0)
            return 0;
        usleep(100 * 1000);
    }
    return -1;
}

static void t_spawn_daemon(void)
{
    snprintf(g_home, sizeof(g_home), "/tmp/airy_memrpc_XXXXXX");
    if (!mkdtemp(g_home)) {
        perror("mkdtemp");
        exit(1);
    }
    setenv("AIRY_HOME", g_home, 1);

    g_daemon = fork();
    assert(g_daemon >= 0);
    if (g_daemon == 0) {
        execl(AIRY_TEST_MEMD_BIN, "mem_d", (char *)NULL);
        _exit(127);
    }
    assert(t_wait_socket(airy_runtime_dir_socket("mem.sock")) == 0);
    assert(daemon_ipc_ops_init("test_memrpc") == AIRY_EOK);
}

static void t_stop_daemon(void)
{
    if (g_daemon > 0) {
        kill(g_daemon, SIGTERM);
        waitpid(g_daemon, NULL, 0);
        g_daemon = -1;
    }
}

/* 断言/崩溃时回收 daemon，防止孤儿进程持有 ctest 输出管道 */
static void t_on_fatal(int sig)
{
    t_stop_daemon();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void t_fatal_hooks(void)
{
    struct sigaction sa;

    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_handler = t_on_fatal;
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

static void test_provider_full(void)
{
    printf("  test_provider_full...\n");

    assert(airy_memrpc_init(NULL) == AIRY_EOK);
    airy_memory_provider_t *p = airy_memory_provider_get_active();
    assert(p != NULL);
    assert(strcmp(p->name, "memrpc") == 0);
    assert(p->has_active_sync(p) == 0);

    char *rid = NULL;
    assert(p->write_raw(p, "hello memrpc world", 18, NULL, &rid) == AIRY_EOK);
    assert(rid != NULL && rid[0] != '\0');

    void *data = NULL;
    size_t len = 0;
    assert(p->get_raw(p, rid, &data, &len) == AIRY_EOK);
    assert(len == 18);
    assert(memcmp(data, "hello memrpc world", 18) == 0);
    AIRY_FREE(data);

    char *rid2 = NULL;
    assert(p->write_raw(p, "tagged entry", 12, "{\"src\":\"test\"}", &rid2) ==
           AIRY_EOK);
    assert(rid2 != NULL);

    char **ids = NULL;
    float *scores = NULL;
    size_t count = 0;
    assert(p->query(p, "hello", 10, &ids, &scores, &count) == AIRY_EOK);
    assert(count >= 1);
    airy_memory_provider_free_query_results(ids, scores, count);

    airy_memory_stats_t stats;
    assert(p->stats(p, &stats) == AIRY_EOK);
    assert(strcmp(stats.provider_name, "memrpc") == 0);
    assert(stats.total_records >= 2);

    assert(p->mount(p, rid, NULL) == AIRY_EOK);
    assert(p->add_memory(p, "extra content", 13) == AIRY_EOK);
    assert(p->evolve(p, 0) == AIRY_EOK);
    assert(p->forget(p) == AIRY_EOK);

    char *health = NULL;
    assert(p->health_check(p, &health) == AIRY_EOK);
    assert(health != NULL && health[0] != '\0');
    AIRY_FREE(health);

    assert(p->retrieve(p, "memrpc", 5, &ids, &scores, &count) == AIRY_EOK);
    airy_memory_provider_free_query_results(ids, scores, count);

    assert(p->delete_raw(p, rid2) == AIRY_EOK);
    assert(p->get_raw(p, rid2, &data, &len) != AIRY_EOK);

    AIRY_FREE(rid);
    printf("    PASSED\n");
}

static void test_direct_evolve_rpc(void)
{
    printf("  test_direct_evolve_rpc...\n");

    const airy_ipc_ops_t *ops = are_ops_get_ipc();
    assert(ops != NULL && ops->rpc_call != NULL);
    char *result = NULL;
    assert(ops->rpc_call(airy_runtime_dir_socket("mem.sock"), "evolve",
                         "{\"query\":\"hello\"}", &result,
                         5000) == AIRY_EOK);
    assert(result != NULL && result[0] != '\0');
    AIRY_FREE(result);
    printf("    PASSED\n");
}

static void test_init_idempotent(void)
{
    printf("  test_init_idempotent...\n");

    assert(airy_memrpc_init(NULL) == AIRY_EOK);
    assert(airy_memory_provider_get_active() != NULL);
    printf("    PASSED\n");
}

int main(void)
{
    printf("test_memrpc: memrpc provider E2E (daemon: %s)\n",
           AIRY_TEST_MEMD_BIN);
    t_fatal_hooks();
    t_spawn_daemon();

    test_provider_full();
    test_direct_evolve_rpc();
    test_init_idempotent();

    t_stop_daemon();
    printf("ALL PASSED\n");
    return 0;
}

#endif /* POSIX */
