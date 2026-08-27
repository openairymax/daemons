// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ledger_handlers.h
 * @brief Context-ledger + prompt-compression RPC handlers.
 */

#ifndef LEDGER_HANDLERS_H
#define LEDGER_HANDLERS_H

#include "platform.h"
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上下文台账（mem.ledger_*，0.1.5） */
void handle_ledger_append(cJSON *params, int id, airy_sock_t fd);
void handle_ledger_window(cJSON *params, int id, airy_sock_t fd);
void handle_ledger_budget(cJSON *params, int id, airy_sock_t fd);
void handle_ledger_mark(cJSON *params, int id, airy_sock_t fd);
void handle_ledger_history(cJSON *params, int id, airy_sock_t fd);
void handle_ledger_stats(int id, airy_sock_t fd);

/* 提示词压缩（mem.compress，0.1.5） */
void handle_compress(cJSON *params, int id, airy_sock_t fd);

#ifdef __cplusplus
}
#endif

#endif /* LEDGER_HANDLERS_H */
