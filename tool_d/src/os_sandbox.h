// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
//
// @file os_sandbox.h
// @brief shell_run OS 级沙箱接口（对标 Codex linux-sandbox）
//
// 设计说明：
// - 在子进程 fork 后、exec 前应用（Landlock restrict_self / seccomp 过滤
//   均不可逆，只影响当前进程及其后代，tool_d 主进程不受影响）。
// - 三级防线：
//   1) Landlock（内核文件系统沙箱）：白名单语义，仅允许显式放行的路径，
//      deny-by-default，阻止 shell 命令写系统目录/越权访问文件。
//   2) seccomp BPF 黑名单：禁止 mount/umount/ptrace/unshare/setns 等
//      特权或命名空间相关 syscall（防提权/逃逸）。
//   3) rlimit：RLIMIT_AS/NOFILE/NPROC/CPU/CORE 资源上限（防资源耗尽）。
// - 平台支持：仅 Linux（Landlock 需内核 >= 5.13，WSL2 6.x / 主流发行版
//   默认开启）；macOS/Windows 编译为空实现（返回不支持）。

#ifndef AIRY_RT_TOOL_OS_SANDBOX_H
#define AIRY_RT_TOOL_OS_SANDBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 沙箱模式（对标 Codex linux-sandbox 的 Landlock 文件系统沙箱）：
 * - OFF：不启用 OS 级隔离（仅保留既有超时/输出截断），用于无沙箱内核平台
 * - WORKSPACE：全局只读 + 工作区可写（默认；shell 命令可读系统/读工作区，
 *   但任何对系统目录的写入/删除均被 Landlock 拒绝）
 * - STRICT：仅系统基础路径 + 工作区可读可执行，工作区可写；网络默认禁用 */
typedef enum {
    OS_SANDBOX_MODE_OFF = 0,
    OS_SANDBOX_MODE_WORKSPACE = 1,
    OS_SANDBOX_MODE_STRICT = 2,
} os_sandbox_mode_t;

typedef struct {
    os_sandbox_mode_t mode;          /* 沙箱模式 */
    char workspace[1024];            /* 允许读写的项目工作目录（绝对路径） */
    int net_access;                  /* 1=允许网络（WORKSPACE 默认 1 / STRICT 默认 0） */
    uint64_t mem_limit_mb;           /* RLIMIT_AS 上限（0=不限制） */
    uint32_t nofile_limit;           /* RLIMIT_NOFILE（0=不限制） */
    uint32_t nproc_limit;            /* RLIMIT_NPROC（0=不限制） */
    uint32_t cpu_limit_sec;          /* RLIMIT_CPU（0=不限制） */
} os_sandbox_cfg_t;

/* 探测当前内核是否支持 Landlock（Linux 且 syscall 可用返回 1，否则 0） */
int os_sandbox_landlock_available(void);

/* 由环境变量构造默认配置：
 *   AIRY_TOOL_SANDBOX_MODE=off|workspace|strict （默认 workspace）
 *   AIRY_TOOL_SANDBOX_WORKSPACE=<绝对路径>     （默认 getcwd）
 *   AIRY_TOOL_SANDBOX_NET=0|1                   （覆盖 net_access） */
void os_sandbox_cfg_from_env(os_sandbox_cfg_t *cfg);

/* fork 后、exec 前调用：应用 rlimit + seccomp + Landlock。
 * 返回 0 成功；失败返回负值（fail-closed：调用方应拒绝执行）。
 * 注意：mode==STRICT 且 Landlock 不可用时返回失败（严格模式不允许降级）；
 * mode==WORKSPACE 且 Landlock 不可用时降级执行（返回 0，日志记录警告）。 */
int os_sandbox_apply(const os_sandbox_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_OS_SANDBOX_H */
