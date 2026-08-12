/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file param_validator.h
 * @brief JSON-RPC parameter validation helpers (unified logic, removes dup).
 */

#ifndef AIRY_RT_PARAM_VALIDATOR_H
#define AIRY_RT_PARAM_VALIDATOR_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validate that a JSON object field exists and has the right type.
 * @param obj JSON object
 * @param key Field name
 * @param expected_type Expected type
 * @return true if present and correctly typed
 */
static inline bool validate_json_field(cJSON *obj, const char *key, int expected_type)
{
    cJSON *field = cJSON_GetObjectItem(obj, key);
    return (field != NULL && field->type == expected_type);
}

/**
 * @brief Get a string field (with default).
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default value
 * @return String value or default
 */
static inline const char *get_string_field(cJSON *obj, const char *key, const char *default_value)
{
    cJSON *field = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsString(field) && field->valuestring) ? field->valuestring : default_value;
}

/**
 * @brief Get an integer field (with default).
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default value
 * @return Integer value or default
 */
static inline int get_int_field(cJSON *obj, const char *key, int default_value)
{
    cJSON *field = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsNumber(field)) ? field->valueint : default_value;
}

/**
 * @brief Get a double field (with default).
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default value
 * @return Floating-point value or default
 */
static inline double get_double_field(cJSON *obj, const char *key, double default_value)
{
    cJSON *field = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsNumber(field)) ? field->valuedouble : default_value;
}

/**
 * @brief Get a boolean field (with default).
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default value
 * @return Boolean value or default
 */
static inline bool get_bool_field(cJSON *obj, const char *key, bool default_value)
{
    cJSON *field = cJSON_GetObjectItem(obj, key);
    if (field == NULL)
        return default_value;
    if (field->type == cJSON_True)
        return true;
    if (field->type == cJSON_False)
        return false;
    return default_value;
}

/**
 * @brief Validate that required fields are present.
 * @param obj JSON object
 * @param ... Required field-name list (NULL-terminated)
 * @return 0 on success, -1 on failure
 */
int validate_required_fields(cJSON *obj, ...);

/**
 * @brief Validate a request ID and return its integer value.
 * @param id JSON request ID field
 * @return Request ID integer value
 */
static inline int get_request_id(cJSON *id)
{
    return cJSON_IsNumber(id) ? id->valueint : 0;
}

/**
 * @brief Validate the basic structure of a JSON-RPC request.
 * @param req JSON request object
 * @param jsonrpc jsonrpc field
 * @param method method field
 * @param params params field
 * @param id id field
 * @return 0 on success, -1 on failure
 */
int validate_jsonrpc_request(cJSON *req, cJSON **jsonrpc, cJSON **method, cJSON **params,
                             cJSON **id);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_PARAM_VALIDATOR_H */
