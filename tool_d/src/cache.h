/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cache.h
 * @brief 工具结果缓存接口（复用 llm_d 的 cache 实现）
 */

#ifndef TOOL_CACHE_H
#define TOOL_CACHE_H

#include "tool_service.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_cache tool_cache_t;

tool_cache_t *tool_cache_create(size_t capacity, int ttl_sec);
void tool_cache_destroy(tool_cache_t *cache);
int tool_cache_get(tool_cache_t *cache, const char *key, char **out_value);
void tool_cache_put(tool_cache_t *cache, const char *key, const char *value);
void tool_cache_clear(tool_cache_t *cache);
char *tool_cache_key(const char *tool_id, const char *params_json, const char *agent_id);
tool_result_t *tool_result_from_json(const char *json);
char *tool_result_to_json(const tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CACHE_H */