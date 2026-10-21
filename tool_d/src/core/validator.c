// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file validator.c
 * @brief Tool parameter-validator implementation (cJSON Schema based).
 */

#include "svc_logger.h"
#include "core/validator.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdlib.h>
#include <string.h>

/* 参数字符串长度上限（0.1.14 由 8192 提升）：1MB 与 gateway 默认
 * max_request_size 对齐。8192 上限使大文档 fs_write 必然被拒
 * （"string exceeds max length 8192"，0.1.13 实机回归实锤），
 * 文档增删改查场景无法工作；daemon RPC 服务端请求接收已支持
 * 动态累积（daemon_main.h），gateway→tool_d 链路无 8KB 限制。 */
#define MAX_STRING_LEN (1024 * 1024)
#define MAX_ARRAY_LEN 256

struct tool_validator {
    int strict_mode;
};

tool_validator_t *tool_validator_create(void)
{
    tool_validator_t *val = AIRY_CALLOC(1, sizeof(tool_validator_t));
    if (val)
        val->strict_mode = 1;
    return val;
}

void tool_validator_destroy(tool_validator_t *val)
{
    AIRY_FREE(val);
}

/* 类型不匹配告警：expected 来自 schema，actual 由 value 实时判定。 */
static int val_mismatch(const char *name, const char *expected, const cJSON *value)
{
    SVC_LOG_WARN("Parameter '%s': expected type '%s', got '%s'", name ? name : "?",
                 expected ? expected : "?",
                 cJSON_IsNumber(value) ? "number" :
                 cJSON_IsBool(value)   ? "boolean" :
                 cJSON_IsArray(value)  ? "array" :
                 cJSON_IsObject(value) ? "object" :
                 cJSON_IsString(value) ? "string" : "null");
    return 0;
}

/* 读取 schema 中的数值约束；约束缺失或非数字时返回 0。 */
static int schema_num(const cJSON *schema, const char *key, double *out)
{
    cJSON *item = cJSON_GetObjectItem(schema, key);
    if (!cJSON_IsNumber(item))
        return 0;
    *out = item->valuedouble;
    return 1;
}

/* 各 JSON 类型的校验器：返回值 1 通过、0 拒绝（已告警）。 */
static int val_string(const char *name, const cJSON *schema, const cJSON *value)
{
    double lo, hi;

    if (!cJSON_IsString(value))
        return val_mismatch(name, "string", value);
    size_t len = strlen(value->valuestring);
    if (schema_num(schema, "minLength", &lo) && len < (size_t)lo) {
        SVC_LOG_WARN("Parameter '%s': string length %zu < minLength %.0f", name, len, lo);
        return 0;
    }
    if (schema_num(schema, "maxLength", &hi) && len > (size_t)hi) {
        SVC_LOG_WARN("Parameter '%s': string length %zu > maxLength %.0f", name, len, hi);
        return 0;
    }
    if (len > MAX_STRING_LEN) {
        SVC_LOG_WARN("Parameter '%s': string exceeds max length %d", name, MAX_STRING_LEN);
        return 0;
    }
    return 1;
}

static int val_number(const char *name, const cJSON *schema, const cJSON *value)
{
    cJSON *type_item = cJSON_GetObjectItem(schema, "type");
    const char *want = cJSON_IsString(type_item) ? type_item->valuestring : "number";
    double lo, hi;

    if (!cJSON_IsNumber(value))
        return val_mismatch(name, want, value);
    if (strcmp(want, "integer") == 0 &&
        value->valuedouble != (double)(int64_t)value->valuedouble) {
        SVC_LOG_WARN("Parameter '%s': not an integer value", name);
        return 0;
    }
    if (schema_num(schema, "minimum", &lo) && value->valuedouble < lo) {
        SVC_LOG_WARN("Parameter '%s': value %g < minimum %g", name, value->valuedouble, lo);
        return 0;
    }
    if (schema_num(schema, "maximum", &hi) && value->valuedouble > hi) {
        SVC_LOG_WARN("Parameter '%s': value %g > maximum %g", name, value->valuedouble, hi);
        return 0;
    }
    return 1;
}

static int val_bool(const char *name, const cJSON *schema, const cJSON *value)
{
    (void)schema;
    return cJSON_IsBool(value) ? 1 : val_mismatch(name, "boolean", value);
}

static int val_array(const char *name, const cJSON *schema, const cJSON *value)
{
    double max_items;

    if (!cJSON_IsArray(value))
        return val_mismatch(name, "array", value);
    int len = cJSON_GetArraySize(value);
    if (schema_num(schema, "maxItems", &max_items) && len > max_items) {
        SVC_LOG_WARN("Parameter '%s': array length %d > maxItems %.0f", name, len, max_items);
        return 0;
    }
    if (len > MAX_ARRAY_LEN) {
        SVC_LOG_WARN("Parameter '%s': array exceeds max items %d", name, MAX_ARRAY_LEN);
        return 0;
    }
    return 1;
}

static int val_object(const char *name, const cJSON *schema, const cJSON *value)
{
    (void)schema;
    return cJSON_IsObject(value) ? 1 : val_mismatch(name, "object", value);
}

static const struct val_rule {
    const char *type;
    int (*check)(const char *name, const cJSON *schema, const cJSON *value);
} VAL_RULES[] = {
    {"string", val_string}, {"number", val_number}, {"integer", val_number},
    {"boolean", val_bool},  {"array", val_array},   {"object", val_object},
};

static int validate_single_param(const char *param_name, const char *schema_str, const cJSON *value)
{
    if (!schema_str || !value)
        return 1;

    CJSON_PARSE_GUARD(schema, schema_str, { return 1; });
    cJSON *type_item = cJSON_GetObjectItem(schema, "type");
    if (!cJSON_IsString(type_item))
        return 1;

    for (size_t i = 0; i < sizeof(VAL_RULES) / sizeof(VAL_RULES[0]); i++)
        if (strcmp(type_item->valuestring, VAL_RULES[i].type) == 0)
            return VAL_RULES[i].check(param_name, schema, value);
    return 1;
}

int tool_validator_validate(tool_validator_t *val,
                            const tool_metadata_t *meta, const char *params_json)
{
    if (!meta || !params_json)
        return AIRY_ERR_INVALID_PARAM;

    CJSON_PARSE_GUARD(root, params_json, {
        SVC_LOG_WARN("Invalid JSON params for tool %s", meta->id);
        return 0;
    });

    if (meta->param_count > 0 && meta->params) {
        for (size_t i = 0; i < meta->param_count; ++i) {
            const char *pname = meta->params[i].name;
            cJSON *item = cJSON_GetObjectItem(root, pname);

            /* Optional params omitted (e.g. fs_list.path) are skipped -
             * consistent with the gateway tool schema's required array
             * (SSoT, T2 fix); only a missing required param is rejected. */
            if (!item) {
                if (meta->params[i].required) {
                    SVC_LOG_WARN("Missing required parameter '%s' for tool %s", pname, meta->id);

                    return 0;
                }
                continue;
            }

            if (!validate_single_param(pname, meta->params[i].schema, item)) {
                SVC_LOG_WARN("Validation failed for parameter '%s' in tool %s", pname, meta->id);

                return 0;
            }
        }
    }

    return 1;
}
