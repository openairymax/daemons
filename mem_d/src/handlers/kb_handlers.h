// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file kb_handlers.h
 * @brief Knowledge-base RPC handlers: kb_ingest, kb_search, kb_delete, kb_list.
 */

#ifndef KB_HANDLERS_H
#define KB_HANDLERS_H

#include "platform.h"
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

void handle_kb_ingest(cJSON *params, int id, airy_sock_t fd);
void handle_kb_search(cJSON *params, int id, airy_sock_t fd);
void handle_kb_delete(cJSON *params, int id, airy_sock_t fd);
void handle_kb_list(cJSON *params, int id, airy_sock_t fd);

#ifdef __cplusplus
}
#endif

#endif /* KB_HANDLERS_H */
