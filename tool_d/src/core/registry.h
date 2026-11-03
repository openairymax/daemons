/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file registry.h
 * @brief Tool-registry interface.
 */

#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include "core/config.h"
#include "tool_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_registry tool_registry_t;

tool_registry_t *tool_registry_create(const tool_config_t *cfg);
void tool_registry_destroy(tool_registry_t *reg);

int tool_registry_add(tool_registry_t *reg, const tool_metadata_t *meta);
tool_metadata_t *tool_registry_get(tool_registry_t *reg, const char *tool_id);
char *tool_registry_list_json(tool_registry_t *reg);

/* 会话内动态工具替换：仅替换已存在的非 builtin 条目（脚本工具重定义）。
 * 返回 AIRY_ERR_UNKNOWN（不存在）或 AIRY_ERR_PERMISSION_DENIED（目标为
 * builtin: 前缀，禁止被脚本抢占）。 */
int reg_replace(tool_registry_t *reg, const tool_metadata_t *meta);

/* 受控名字遍历：锁内逐工具回调（仅含经 D8 谓词可暴露者）。供诊断面
 * （未知工具近似建议）使用；回调不得反向获取 registry 锁。 */
typedef void (*tool_name_fn)(const char *tool_id, void *ud);
void reg_each_name(tool_registry_t *reg, tool_name_fn fn, void *ud);

/* D8 可见性谓词：返回非 0 表示该工具此刻可暴露给 agent。谓词在 registry
 * 锁内被调用，实现方须避免反向获取 registry 锁。 */
typedef int (*tool_up_fn)(const char *tool_id, void *ud);
void tool_registry_filter(tool_registry_t *reg, tool_up_fn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
