// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file cache_handlers.h
 * @brief Semantic-cache RPC handlers: cache_put, cache_get, cache_del, cache_stats.
 */

#ifndef CACHE_HANDLERS_H
#define CACHE_HANDLERS_H

#include "platform.h"
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

void handle_cache_put(cJSON *params, int id, airy_sock_t fd);
void handle_cache_get(cJSON *params, int id, airy_sock_t fd);
void handle_cache_del(cJSON *params, int id, airy_sock_t fd);
void handle_cache_stats(int id, airy_sock_t fd);

#ifdef __cplusplus
}
#endif

#endif /* CACHE_HANDLERS_H */
