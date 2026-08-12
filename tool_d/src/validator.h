/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file validator.h
 * @brief Tool parameter-validator interface.
 */

#ifndef TOOL_VALIDATOR_H
#define TOOL_VALIDATOR_H

#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_validator tool_validator_t;

tool_validator_t *tool_validator_create(void);
void tool_validator_destroy(tool_validator_t *val);

/**
 * @brief Validate that parameters match the tool definition.
 * @param val Validator
 * @param meta Tool metadata
 * @param params_json Parameter string
 * @return 1 valid, 0 invalid
 */
int tool_validator_validate(tool_validator_t *val, const tool_metadata_t *meta,
                            const char *params_json);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_VALIDATOR_H */