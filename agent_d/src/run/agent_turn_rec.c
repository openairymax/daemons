/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_turn_rec.c
 * @brief 会话记忆记录构造（读写契约实现，纯函数，无传输依赖）。
 */

#include "agent_turn_rec.h"

static void rec_add(cJSON *arr, const char *conv, const char *role, const char *text)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *meta = cJSON_CreateObject();

    if (!params || !meta) {
        cJSON_Delete(params);
        cJSON_Delete(meta);
        return;
    }
    cJSON_AddStringToObject(params, "data", text);
    cJSON_AddStringToObject(meta, "conv", conv ? conv : "");
    cJSON_AddStringToObject(meta, "session_id", conv ? conv : "");
    cJSON_AddStringToObject(meta, "role", role);
    cJSON_AddItemToObject(params, "metadata", meta);
    cJSON_AddItemToArray(arr, params);
}

cJSON *agent_turn_rec(const char *conv, const char *user, const char *reply)
{
    if (!user || !*user)
        return NULL;

    cJSON *arr = cJSON_CreateArray();

    if (!arr)
        return NULL;
    rec_add(arr, conv, "user", user);
    if (reply && *reply)
        rec_add(arr, conv, "assistant", reply);
    return arr;
}
