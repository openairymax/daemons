/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* */
/* @file os_sandbox.h */

/* */


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
    os_sandbox_mode_t mode;
    char workspace[1024];
    int net_access;
    uint64_t mem_limit_mb;
    uint32_t nofile_limit;
    uint32_t nproc_limit;
    uint32_t cpu_limit_sec;
} os_sandbox_cfg_t;


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
