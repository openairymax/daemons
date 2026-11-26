/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_priv_internal.h
 * @brief 特权执行通道（priv_exec）：显式授权下按白名单执行系统管理程序。
 *
 * shell_run 的子进程一律套 OS 级沙箱（Landlock + no_new_privs），提权在
 * 结构上不可达（platform_sandbox.c:143），于是包安装一类需要 root 的系统
 * 动作在 agent 内无法完成。本域给出唯一显式通道，四个约束同时成立：
 *
 *  - 程序白名单：只认编译期表内的绝对路径，模型不可传任意命令；
 *  - 参数校验：按程序族逐项判定，拒绝 shell 元字符、路径穿越与越权参数；
 *  - 失败关闭：白名单未命中、参数越权、通道关闭、提权不可用一律拒绝；
 *  - 审计事件：每次尝试（含拒绝）落 daemon_audit_log_event，带判定结果。
 *
 * 通道开关 AIRY_PRIVILEGED_EXEC=off 回到全禁（默认开启，仍需 ACL 与审批）。
 * 与 shell_run 的差别只在「不套沙箱」：输出捕获、超时与截断复用
 * builtin_proc_capture，审计与会话边界仍由上层 gate/ACL 把住。
 */

#ifndef AIRY_RT_TOOL_BUILTIN_PRIV_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_PRIV_INTERNAL_H

#include "tool_service_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRIV_TOOL_NAME "priv_exec"
#define PRIV_ENV_SWITCH "AIRY_PRIVILEGED_EXEC"
/* 用户态安装下守护以普通用户运行，特权位经 sudo -n 切换（sudoers 未授权
 * 时立即失败，不触发密码提示）。AIRY_PRIV_SUDO 可覆盖 sudo 路径：容器内
 * 布局差异，以及测试注入不存在路径以确定性覆盖「提权不可用即拒绝」。 */
#define PRIV_SUDO_PATH "/usr/bin/sudo"
#define PRIV_ENV_SUDO "AIRY_PRIV_SUDO"

/* 单次调用的参数规模上界：包名与路径远小于此，超界即判越权。 */
#define PRIV_MAX_ARGS 32
#define PRIV_ARG_MAX_LEN 256
#define PRIV_PROG_MAX_LEN 128

/* 程序族决定参数形状：包管理器验收子命令与包名，服务管理器验收
 * 单元名与动作。族内程序共用同一套校验器，族间不共享。 */
typedef enum {
    PRIV_FAMILY_PKG = 0,
    PRIV_FAMILY_SVC = 1,
    PRIV_FAMILY_COUNT
} priv_family_t;

typedef struct {
    const char *name;   /* 模型可见的短名（不含路径） */
    const char *path;   /* 白名单绝对路径，exec 前核对存在性 */
    priv_family_t family;
} priv_prog_t;

/* 装配结果：program 指向白名单表内静态项，argv[0] 为程序短名，
 * argv[1..argc] 为堆分配的实参，argv[argc+1] 恒为 NULL。 */
typedef struct {
    const priv_prog_t *program;
    int argc;
    char *argv[PRIV_MAX_ARGS + 2];
} priv_plan_t;

/* 通道开关：默认开启；off/0/false/no 关闭（大小写不敏感）。 */
int priv_enabled(void);

/* 按短名查白名单；未命中返回 NULL（调用方据此判 EXEC_NOT_FOUND）。 */
const priv_prog_t *priv_lookup(const char *name);

/* 纯函数：解析 params_json 并完成白名单与参数校验，装配 plan。
 * 成功 AIRY_OK；非白名单 AIRY_ERR_EXEC_NOT_FOUND；参数越权
 * AIRY_ERR_EXEC_ARGS；params 非法 AIRY_ERR_INVALID_PARAM。
 * 不派生进程、不读环境、不落审计，便于非 root 环境下直接覆盖三路拒绝。 */
int priv_plan_build(const char *params_json, priv_plan_t *plan, tool_result_t *res);

/* 释放 plan 中堆分配的实参（program 与 argv[0] 为静态项，不释放）。 */
void priv_plan_free(priv_plan_t *plan);

/* 工具入口：通道开关 + euid 检查 + plan 装配 + 审计 + 出沙箱执行。
 * 拒绝路径统一返回 AIRY_ERR_EXEC_PERMISSION；执行结果按
 * builtin_proc_capture 的成功/失败口径回填 err_code 与 exit_code。 */
int priv_exec_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_PRIV_INTERNAL_H */
