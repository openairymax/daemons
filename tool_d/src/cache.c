// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file cache.c
 * @brief Tool-result cache key builder and JSON (de)serialization.
 *
 * The LRU/TTL storage itself is provided by commons cache_common
 * (cache_create_string_cache / cache_get_string / cache_put_string);
 * tool_d only keeps the tool-specific key composition and the
 * tool_result_t JSON mapping.
 */

#include "cache.h"
#include "error.h"
#include "memory_common.h"
#include "tool_service.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <string.h>

char *tool_cache_key(const char *tool_id, const char *params_json, const char *agent_id)
{
    if (!tool_id || !params_json) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    /* Include agent_id in the cache key (SEC: prevents an unauthorized agent
     * from hitting another's approval-cleared cache, bypassing permission
     * approval). Empty agent_id degrades to "tool_d|tool|params" for legacy
     * compatibility. */
    const char *subject = (agent_id && agent_id[0]) ? agent_id : "tool_d";

    size_t tool_id_len = strlen(tool_id);
    size_t params_len = strlen(params_json);
    size_t subject_len = strlen(subject);
    size_t len = subject_len + tool_id_len + params_len + 3;

    char *key = memory_safe_alloc(len);
    if (!key) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    snprintf(key, len, "%s|%s|%s", subject, tool_id, params_json);
    return key;
}

tool_result_t *tool_result_from_json(const char *json)
{

    CJSON_PARSE_GUARD(root, json, { AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed"); });
    tool_result_t *res = AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!res) {

        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (cJSON_IsNumber(success))
        res->success = success->valueint;
    cJSON *output = cJSON_GetObjectItem(root, "output");
    if (cJSON_IsString(output))
        res->output = memory_safe_strdup(output->valuestring);
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(error))
        res->error = memory_safe_strdup(error->valuestring);
    cJSON *exit_code = cJSON_GetObjectItem(root, "exit_code");
    if (cJSON_IsNumber(exit_code))
        res->exit_code = exit_code->valueint;

    return res;
}

char *tool_result_to_json(const tool_result_t *res)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "success", res->success);
    if (res->output)
        cJSON_AddStringToObject(root, "output", res->output);
    if (res->error)
        cJSON_AddStringToObject(root, "error", res->error);
    cJSON_AddNumberToObject(root, "exit_code", res->exit_code);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
