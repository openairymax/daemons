// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_tool_avail.c
 * @brief 工具可用性视图单元测试（0.1.19 D8）。
 *
 * 覆盖"能力暴露 ⇒ 依赖可用"闭环：未探测/无依赖视为可用（fail-open）、
 * 依赖不可达即摘除、依赖恢复即回挂。可达性判定经真实 AF_UNIX listener
 * 往返（非桩），在 AIRY_HOME 隔离的临时运行目录内进行。
 */

#include "core/tool_avail.h"
#include "platform_paths.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* ---- 隔离运行目录 ---- */

static char g_home[AIRY_PATH_MAX];
static char g_run[AIRY_PATH_MAX];

static void env_setup(void)
{
#ifndef _WIN32
    char tmpl[] = "/tmp/airy_d8_avail_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);
    snprintf(g_home, sizeof(g_home), "%s", dir);
    setenv("AIRY_HOME", g_home, 1);

    snprintf(g_run, sizeof(g_run), "%s", airy_runtime_dir());
    if (mkdir(g_run, 0755) != 0) {
        /* 目录已存在亦可通过 */
    }

    /* 触发路径解析并锁定 AIRY_HOME（后续 airy_runtime_dir_socket 依赖之） */
    assert(strcmp(airy_home_dir(), g_home) == 0);
#endif
}

static void env_teardown(void)
{
#ifndef _WIN32
    rmdir(g_run);
    rmdir(g_home);
#endif
}

#ifndef _WIN32
/* 在运行目录内建真实 AF_UNIX listener，用于验证"可达"分支。 */
static int listener_up(const char *name)
{
    char path[AIRY_PATH_MAX];
    snprintf(path, sizeof(path), "%s", airy_runtime_dir_socket(name));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void listener_down(int fd, const char *name)
{
    char path[AIRY_PATH_MAX];
    snprintf(path, sizeof(path), "%s", airy_runtime_dir_socket(name));
    close(fd);
    unlink(path);
}
#endif

/* ---- 用例 ---- */

static void test_param_guard(void)
{
    printf("  test_param_guard...\n");

    tool_avail_t *ta = tool_avail_new();
    assert(ta != NULL);

    assert(tool_avail_dep(NULL, "t", "x.sock") != 0);
    assert(tool_avail_dep(ta, NULL, "x.sock") != 0);
    assert(tool_avail_dep(ta, "t", NULL) != 0);
    assert(tool_avail_dep(ta, "", "x.sock") != 0);
    assert(tool_avail_dep(ta, "t", "") != 0);

    /* fail-open：视图缺失 / 工具名为空 一律视为可用 */
    assert(tool_avail_ok(NULL, "t") == 1);
    assert(tool_avail_ok(ta, NULL) == 1);
    assert(tool_avail_ok(ta, "never_declared") == 1);

    tool_avail_free(ta);
    tool_avail_free(NULL);

    printf("    PASSED\n");
}

static void test_unknown_is_available(void)
{
    printf("  test_unknown_is_available...\n");

    tool_avail_t *ta = tool_avail_new();
    assert(ta != NULL);

    /* 声明依赖但尚未探测：unknown ⇒ 可用（不隐藏） */
    assert(tool_avail_dep(ta, "maths_eval", "d8_absent.sock") == 0);
    assert(tool_avail_ok(ta, "maths_eval") == 1);

    tool_avail_free(ta);

    printf("    PASSED\n");
}

static void test_unreachable_drops(void)
{
    printf("  test_unreachable_drops...\n");

    tool_avail_t *ta = tool_avail_new();
    assert(ta != NULL);
    assert(tool_avail_dep(ta, "maths_eval", "d8_absent.sock") == 0);

    tool_avail_poll(ta);
    assert(tool_avail_ok(ta, "maths_eval") == 0);

    /* 无依赖工具不受影响 */
    assert(tool_avail_ok(ta, "fs_read") == 1);

    tool_avail_free(ta);

    printf("    PASSED\n");
}

#ifndef _WIN32
static void test_reachable_and_recovery(void)
{
    printf("  test_reachable_and_recovery...\n");

    const char *live = "d8_live.sock";
    const char *gone = "d8_absent.sock";

    tool_avail_t *ta = tool_avail_new();
    assert(ta != NULL);

    /* 两个工具共享同一依赖 socket，验证 link 表扇入 */
    assert(tool_avail_dep(ta, "maths_eval", live) == 0);
    assert(tool_avail_dep(ta, "maths_stats", live) == 0);
    assert(tool_avail_dep(ta, "maths_plot", gone) == 0);

    int lfd = listener_up(live);
    assert(lfd >= 0);

    tool_avail_poll(ta);
    assert(tool_avail_ok(ta, "maths_eval") == 1);
    assert(tool_avail_ok(ta, "maths_stats") == 1);
    assert(tool_avail_ok(ta, "maths_plot") == 0);

    /* 依赖掉线 ⇒ 摘除；恢复 ⇒ 回挂 */
    listener_down(lfd, live);
    tool_avail_poll(ta);
    assert(tool_avail_ok(ta, "maths_eval") == 0);
    assert(tool_avail_ok(ta, "maths_stats") == 0);

    lfd = listener_up(live);
    assert(lfd >= 0);
    tool_avail_poll(ta);
    assert(tool_avail_ok(ta, "maths_eval") == 1);
    assert(tool_avail_ok(ta, "maths_stats") == 1);
    listener_down(lfd, live);

    tool_avail_free(ta);

    printf("    PASSED\n");
}
#endif

static void test_socket_capacity(void)
{
    printf("  test_socket_capacity...\n");

    tool_avail_t *ta = tool_avail_new();
    assert(ta != NULL);

    char id[32];
    char sock[32];
    int last = 0;
    for (int i = 0; i < 9; i++) {
        snprintf(id, sizeof(id), "t%d", i);
        snprintf(sock, sizeof(sock), "d8_cap_%d.sock", i);
        last = tool_avail_dep(ta, id, sock);
    }
    assert(last != 0); /* 第 9 个不同 socket 超出容量上限 */

    tool_avail_free(ta);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Availability View Unit Tests\n");
    printf("=========================================\n");

    env_setup();

    test_param_guard();
    test_unknown_is_available();
    test_unreachable_drops();
#ifndef _WIN32
    test_reachable_and_recovery();
#endif
    test_socket_capacity();

    env_teardown();

    printf("\nAll tool availability tests PASSED\n");
    return 0;
}
