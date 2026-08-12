// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file param_validator.c
 * @brief JSON-RPC parameter validation tool implementation.
 */

#include "param_validator.h"

#include <stdarg.h>
#include <string.h>
#include "error.h"

/** @brief Validate that required fields are present. */
int validate_required_fields(cJSON *obj, ...)
{
    if (!obj || !cJSON_IsObject(obj)) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "param_validator: null param name");
    }

    va_list args;
    va_start(args, obj);

    const char *field_name;
    while ((field_name = va_arg(args, const char *)) != NULL) {
        cJSON *field = cJSON_GetObjectItem(obj, field_name);
        if (!field) {
            va_end(args);
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "param_validator: name too long");
        }
    }

    va_end(args);
    return 0;
}

/** @brief Validate the basic structure of a JSON-RPC request. */
int validate_jsonrpc_request(cJSON *req, cJSON **jsonrpc, cJSON **method, cJSON **params,
                             cJSON **id)
{
    if (!req || !cJSON_IsObject(req)) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "param_validator: null value");
    }

    *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
    *method = cJSON_GetObjectItem(req, "method");
    *params = cJSON_GetObjectItem(req, "params");
    *id = cJSON_GetObjectItem(req, "id");

    if (!cJSON_IsString(*jsonrpc) || strcmp((*jsonrpc)->valuestring, "2.0") != 0 ||
        !cJSON_IsString(*method) || !(*id)) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "param_validator: value too long");
    }

    return 0;
}
