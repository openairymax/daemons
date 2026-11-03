/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_toolmake_internal.h
 * @brief 会话内自制工具（meta 工具 tool_create）：脚本落盘 + 注册表登记。
 *
 * 工具目录此前是编译期静态集合（16 个内置工具 + 部署期 register RPC /
 * MCP 桥两条通道），模型运行期无法把一次性的处理逻辑沉淀成可复用工具，
 * 只能反复手写 shell_run。本域给出第三条通道：模型在当前会话内调用
 * tool_create，把脚本写入 $AIRY_DATA_DIR/scripts/<name>（0700）并登记为
 * 可执行工具，同一会话的后续轮次即可按名调用。
 *
 * 边界（fail-closed）：
 *  - 名字限定 [A-Za-z0-9_]{1,32}，且不得占用 tool_create 自身；
 *  - 脚本必须以 #! 开头（必须以 shebang 声明解释器，不做猜测），
 *    ≤64KiB，落盘权限 0700；
 *  - 只能覆盖由本机制创建的同路径脚本；内置（builtin: 前缀）与部署期
 *    声明的外部工具一律拒绝覆盖，避免脚本抢占既有能力；
 *  - 登记后自动授予 tool_d 自身 ACL（与内置工具注册同口径）。
 */

#ifndef AIRY_RT_TOOL_BUILTIN_TOOLMAKE_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_TOOLMAKE_INTERNAL_H

#include "tool_service_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 服务实例注入：register_builtin_tools 装配期调用一次；未注入时
 * tool_create 一律拒绝（不允许静默降级成"只写文件不登记"）。 */
void toolmake_attach(tool_service_t *svc);

/* 服务侧两个动作由登记域（service_builtin.c）实现，builtin 机制侧不直接
 * 依赖服务接口头，避免内置工具与 service 实现细节互相牵引。
 * owner: 0=可新建 1=可被本机制替换 -1=已被部署期/内置占用。
 * install: 注册或就地替换并使能 ACL；失败由调用方回滚脚本。 */
int toolmake_svc_owner(tool_service_t *svc, const char *name, const char *path);
int toolmake_svc_install(tool_service_t *svc, const char *name, const char *desc,
                         tool_param_t *params, int pn, const char *path, int updated);

int toolmake_create(const char *params_json, uint32_t timeout_ms, tool_result_t *res);

/* 参数表释放（name/schema 逐项 AIRY_FREE 后释放数组本体）。 */
void make_par_free(tool_param_t *arr, int n);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_TOOLMAKE_INTERNAL_H */
