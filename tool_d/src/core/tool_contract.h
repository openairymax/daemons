// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_contract.h
 * @brief 工具契约边车（sidecar）：为 ABI 冻结的 tool_metadata_t 增补
 *        path_base / sandbox_scope / requires_daemon / creates_parents /
 *        side_effects 五个契约字段，经 list_tools 的 x-airy 块对外发布。
 *
 * 机制与策略分离：本文件只提供"绑定—查找—投影"机制，具体契约表由策略层
 * （rpc/service_builtin.c）声明后调用 tool_contract_bind() 注入。这样 core
 * 无需反向依赖 builtin 内部头，也未向共享结构体添加任何字段。
 */

#ifndef AIRY_TOOL_CONTRACT_H
#define AIRY_TOOL_CONTRACT_H

#include <stddef.h>

struct cJSON;

#define TOOL_FX_FS (1u << 0)
#define TOOL_FX_NET (1u << 1)
#define TOOL_FX_PROC (1u << 2)

#define TOOL_CONTRACT_PATH_MAX 4096

typedef struct tool_contract {
    const char *id;       /* 工具 id，与 tool_metadata_t.id 一致 */
    const char *dep_sock; /* 依赖 daemon 的 socket 文件名；NULL=无外部依赖 */
    const char *dep_name; /* 契约字段 requires_daemon；NULL=无依赖 */
    unsigned fx;          /* TOOL_FX_* 位集 */
    unsigned char mkdirs; /* creates_parents：写文件时自动建父目录 */
    unsigned char fenced; /* 可写文件系统：sandbox_scope 适用 */
} tool_contract_t;

typedef struct tool_contract_env {
    char path_base[TOOL_CONTRACT_PATH_MAX];
} tool_contract_env_t;

/* 启动期绑定一次，必须先于任何 list_tools 调用。 */
void tool_contract_bind(const tool_contract_t *tab, size_t n);
size_t tool_contract_n(void);
const tool_contract_t *tool_contract_at(size_t idx);
const tool_contract_t *tool_contract_of(const char *tool_id);

/* 解析相对路径基准与沙箱根（含 getcwd/realpath，必须在注册表锁外调用）。 */
int tool_contract_env(tool_contract_env_t *env);

/* 向 obj 投影 sandbox_scope 数组（list_tools 的 x-airy 与 policy 错误信封
 * 的 err.ctx 共用同一形状）：fenced!=0 时为 [base]，否则为空数组，表示该
 * 工具不写文件系统。base 为沙箱根（tool_contract_env 的 path_base）。
 * 返回 0 成功；-1 参数非法或分配失败。 */
int tool_contract_scope_put(struct cJSON *obj, const char *base, int fenced);

/* 把契约投影为 obj["x-airy"]；未声明契约的工具（插件）不输出该键。 */
int tool_contract_put(struct cJSON *obj, const char *tool_id,
                      const tool_contract_env_t *env);

#endif /* AIRY_TOOL_CONTRACT_H */
