/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file manager.h
 * @brief Tool-service config structures.
 */

#ifndef TOOL_CONFIG_H
#define TOOL_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *name;
    char *executable;
    char **params;
    int timeout_sec;
    int cacheable;
    char *permission_rule;
} tool_def_t;

typedef struct {
    tool_def_t *tools;
    size_t cache_capacity;
    int cache_ttl_sec;
    int executor_workers;
    char *workbench_type;
    char *container_image;
} tool_config_t;

tool_config_t *tool_config_load(const char *path);
void tool_config_free(tool_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CONFIG_H */