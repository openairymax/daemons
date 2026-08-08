// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
//
// @file test_os_sandbox.c
// @brief os_sandbox（Landlock + seccomp + rlimit）单元测试
//
// 覆盖：
// - Landlock 可用性探测
// - WORKSPACE 模式：系统目录只读、工作区可写、/tmp 可写
// - STRICT 模式：非白名单路径读/写均被拒绝
// - seccomp：unshare/mount 等特权 syscall 返回 EPERM
// - OFF 模式：不施加限制
// - 环境变量构造配置

#include "os_sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("PASS: %s\n", msg);                                         \
        } else {                                                               \
            printf("FAIL: %s\n", msg);                                         \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_ERRNO(cond, msg)                                                 \
    do {                                                                       \
        int _e = errno;                                                        \
        if (cond) {                                                            \
            printf("PASS: %s\n", msg);                                         \
        } else {                                                               \
            printf("FAIL: %s (errno=%d)\n", msg, _e);                          \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* 测试工作区目录 */
static const char *k_ws = "/tmp/airy_os_sandbox_ws";

/* 在沙箱子进程中执行 action，返回子进程退出码（沙箱应用失败=126） */
static int run_in_sandbox(const os_sandbox_cfg_t *cfg, int (*action)(void))
{
    pid_t pid = fork();
    if (pid < 0) {
        return -999;
    }
    if (pid == 0) {
        if (os_sandbox_apply(cfg) != 0) {
            _exit(126);
        }
        int rc = action();
        _exit(rc);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---------- 沙箱内动作（返回 0=动作成功，非 0=动作被拒绝/失败） ---------- */

static int act_echo_ok(void)
{
    return system("echo hello > /dev/null") == 0 ? 0 : 1;
}

static int act_write_workspace(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/probe.txt", k_ws);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 1; /* 被拒绝 */
    }
    close(fd);
    unlink(path);
    return 0;
}

static int act_write_system(void)
{
    /* 写用户主目录（用户有写权限）应被 Landlock 拒绝（工作区外写禁止）。
     * 用 $HOME 而非 /etc：非 root 下 /etc 本就不可写，无法区分沙箱效果。 */
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = "/root";
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/.airy_sandbox_probe_should_fail", home);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
        unlink(path);
        return 0; /* 意外成功（沙箱未生效） */
    }
    return 1;
}

static int act_read_system(void)
{
    /* STRICT 模式：/proc 不在白名单，读应失败；WORKSPACE：全局可读 */
    FILE *f = fopen("/proc/version", "r");
    if (!f) {
        return 1;
    }
    fclose(f);
    return 0;
}

static int act_read_home_ok(void)
{
    /* WORKSPACE 模式：/ 只读放行，读 $HOME 应成功 */
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) {
        return 1;
    }
    fclose(f);
    return 0;
}

static int act_unshare(void)
{
    /* 应被 seccomp 黑名单拒绝（EPERM），返回非 0 表示被拒 */
    return unshare(CLONE_NEWNS) == 0 ? 0 : 1;
}

static int act_mount(void)
{
    /* 应被 seccomp 黑名单拒绝（EPERM） */
    return mount("none", "/tmp", "tmpfs", 0, NULL) == 0 ? 0 : 1;
}

static int act_write_tmp(void)
{
    int fd = open("/tmp/airy_sb_tmp_probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 1;
    }
    close(fd);
    unlink("/tmp/airy_sb_tmp_probe");
    return 0;
}

static int act_ptrace(void)
{
    /* 无沙箱时 PTRACE_TRACEME 对自己应成功；seccomp 黑名单拦截后返回 EPERM */
    return ptrace(PTRACE_TRACEME, 0, NULL, NULL) == 0 ? 0 : 1;
}

/* ---------- 用例 ---------- */

static void test_landlock_available(void)
{
    printf("== test_landlock_available ==\n");
    CHECK(os_sandbox_landlock_available() == 1, "Landlock available on Linux >= 5.13");
}

static void test_env_config(void)
{
    printf("== test_env_config ==\n");
    setenv("AIRY_TOOL_SANDBOX_MODE", "strict", 1);
    setenv("AIRY_TOOL_SANDBOX_NET", "0", 1);
    setenv("AIRY_TOOL_SANDBOX_WORKSPACE", k_ws, 1);
    os_sandbox_cfg_t cfg;
    os_sandbox_cfg_from_env(&cfg);
    CHECK(cfg.mode == OS_SANDBOX_MODE_STRICT, "mode parsed as strict");
    CHECK(cfg.net_access == 0, "strict disables net");
    CHECK(strcmp(cfg.workspace, k_ws) == 0, "workspace parsed");
    unsetenv("AIRY_TOOL_SANDBOX_MODE");
    unsetenv("AIRY_TOOL_SANDBOX_NET");
    unsetenv("AIRY_TOOL_SANDBOX_WORKSPACE");

    /* 默认值 */
    os_sandbox_cfg_from_env(&cfg);
    CHECK(cfg.mode == OS_SANDBOX_MODE_WORKSPACE, "default mode workspace");
    CHECK(cfg.net_access == 1, "default net on (workspace)");
}

static void test_workspace_mode(void)
{
    printf("== test_workspace_mode ==\n");
    os_sandbox_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = OS_SANDBOX_MODE_WORKSPACE;
    cfg.net_access = 1;
    snprintf(cfg.workspace, sizeof(cfg.workspace), "%s", k_ws);

    CHECK(run_in_sandbox(&cfg, act_echo_ok) == 0, "echo works in sandbox");
    CHECK(run_in_sandbox(&cfg, act_write_workspace) == 0, "write to workspace allowed");
    CHECK(run_in_sandbox(&cfg, act_write_tmp) == 0, "write to /tmp allowed");
    CHECK(run_in_sandbox(&cfg, act_write_system) == 1, "write to /etc denied by Landlock");
    CHECK(run_in_sandbox(&cfg, act_read_home_ok) == 0, "read /etc allowed (global read)");
    CHECK(run_in_sandbox(&cfg, act_unshare) == 1, "unshare denied by seccomp");
    CHECK(run_in_sandbox(&cfg, act_mount) == 1, "mount denied by seccomp");
    CHECK(run_in_sandbox(&cfg, act_ptrace) == 1, "ptrace denied by seccomp");
}

static void test_strict_mode(void)
{
    printf("== test_strict_mode ==\n");
    os_sandbox_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = OS_SANDBOX_MODE_STRICT;
    cfg.net_access = 0;
    snprintf(cfg.workspace, sizeof(cfg.workspace), "%s", k_ws);

    CHECK(run_in_sandbox(&cfg, act_echo_ok) == 0, "echo works in strict sandbox");
    CHECK(run_in_sandbox(&cfg, act_write_workspace) == 0, "write to workspace allowed");
    CHECK(run_in_sandbox(&cfg, act_read_system) == 1, "read /proc denied (not whitelisted)");
    CHECK(run_in_sandbox(&cfg, act_write_system) == 1, "write to /etc denied");
    CHECK(run_in_sandbox(&cfg, act_unshare) == 1, "unshare denied by seccomp");
}

static void test_off_mode(void)
{
    printf("== test_off_mode ==\n");
    os_sandbox_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = OS_SANDBOX_MODE_OFF;

    CHECK(run_in_sandbox(&cfg, act_echo_ok) == 0, "echo works without sandbox");
    CHECK(run_in_sandbox(&cfg, act_write_tmp) == 0, "write to /tmp allowed without sandbox");
    CHECK(run_in_sandbox(&cfg, act_ptrace) == 0, "ptrace allowed without sandbox");
}

int main(void)
{
    /* 准备测试工作区 */
    mkdir(k_ws, 0755);

    test_landlock_available();
    test_env_config();
    test_workspace_mode();
    test_strict_mode();
    test_off_mode();

    /* 清理测试工作区 */
    rmdir(k_ws);

    printf("\n%s: %d failures\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
