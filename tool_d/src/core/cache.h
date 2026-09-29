/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cache.h
 * @brief Tool-result cache key builder and JSON (de)serialization helpers.
 *
 * The LRU/TTL storage itself lives in commons cache_common (B16 表二 #1
 * 下沉)：tool_service holds a cache_t created via
 * cache_create_string_cache(); only the tool-specific helpers stay here.
 */

#ifndef TOOL_CACHE_H
#define TOOL_CACHE_H

#include "tool_service.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char *tool_cache_key(const char *tool_id, const char *params_json, const char *agent_id);
tool_result_t *tool_result_from_json(const char *json);
char *tool_result_to_json(const tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CACHE_H */
