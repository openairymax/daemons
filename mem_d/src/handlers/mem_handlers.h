// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_handlers.h
 * @brief Core memory RPC handlers: write, search, get, delete, count,
 *        recent, evolve, health_check, get_stats.
 */

#ifndef MEM_HANDLERS_H
#define MEM_HANDLERS_H

#include "platform.h"
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

void handle_write(cJSON *params, int id, airy_sock_t fd);
void handle_search(cJSON *params, int id, airy_sock_t fd);
void handle_get(cJSON *params, int id, airy_sock_t fd);
void handle_delete(cJSON *params, int id, airy_sock_t fd);
void handle_count(int id, airy_sock_t fd);
void handle_recent(cJSON *params, int id, airy_sock_t fd);
void handle_evolve(cJSON *params, int id, airy_sock_t fd);
void handle_health_check(int id, airy_sock_t fd);
void handle_get_stats(int id, airy_sock_t fd);

#ifdef __cplusplus
}
#endif

#endif /* MEM_HANDLERS_H */
