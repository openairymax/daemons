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
int tool_registry_remove(tool_registry_t *reg, const char *tool_id);
tool_metadata_t *tool_registry_get(tool_registry_t *reg, const char *tool_id);
char *tool_registry_list_json(tool_registry_t *reg);

/* D8 可见性谓词：返回非 0 表示该工具此刻可暴露给 agent。谓词在 registry
 * 锁内被调用，实现方须避免反向获取 registry 锁。 */
typedef int (*tool_up_fn)(const char *tool_id, void *ud);
void tool_registry_filter(tool_registry_t *reg, tool_up_fn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
