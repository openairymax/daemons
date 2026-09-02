/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file validator_cjson.h
 * @brief cJSON 规则校验器（validator_* API）——daemons 权威实现。
 *
 * 0.1.9 0c 复核澄清：本组件与 commons/utils/security 的 input_validator
 * （airy_validate_* 字符串/路径/URL 安全校验 API）是**两个不同组件**，
 * 并非重复实现，故不合并、不迁移。当前仅被 daemons/common/tests 消费。
 *
 * 0.1.9 M0-L2 更名：原名 input_validator.h 与 commons 权威头同名，且两者
 * 曾共用 include guard，同一 TU 先后包含时后者被静默跳过。现以独立 guard
 * 与独立文件名彻底解耦。
 */

#ifndef AIRY_RT_VALIDATOR_CJSON_H
#define AIRY_RT_VALIDATOR_CJSON_H

#include <cjson/cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RULES 64

typedef enum {
    VALIDATE_REQUIRED = 0,
    VALIDATE_STRING,
    VALIDATE_INT,
    VALIDATE_PATTERN,
    VALIDATE_SANITIZE,
    VALIDATE_CUSTOM
} validation_rule_type_t;

typedef int (*custom_validator_fn)(const cJSON *item, char **out_error);

typedef struct {
    validation_rule_type_t type;
    bool required;
    size_t min_len;
    size_t max_len;
    char *field_name;
    const char *pattern;
    custom_validator_fn custom_validator;
    unsigned int sanitize_flags;
} validation_rule_t;

typedef struct validation_result {
    int valid;
    char *error_message;
    char *error_field;
    validation_rule_t rules[MAX_RULES];
    int rule_count;
} validation_result_t;

validation_result_t *validator_create(void);
void validator_destroy(validation_result_t *validator);
int validator_add_rule(validation_result_t *validator, const validation_rule_t *rule);
validation_result_t *validator_validate(validation_result_t *validator, const cJSON *data);
int validate_required_field(const cJSON *obj, const char *field, char **out_error);
int validate_string_field(const cJSON *obj, const char *field, size_t min_len, size_t max_len,
                          char **out_error);

int validate_sanitized_string(const cJSON *obj, const char *field, unsigned int sanitize_flags,
                              char **out_error);
int security_check_string(const char *input, unsigned int flags, char **out_violation);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_VALIDATOR_CJSON_H */
