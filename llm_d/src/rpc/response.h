/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file response.h
 * @brief Response serialization interface.
 */

#ifndef AIRY_RT_LLM_RESPONSE_H
#define AIRY_RT_LLM_RESPONSE_H

/* B16-S2 内层反依赖：序列化层只依赖跨层类型契约（commons SSoT）。 */
#include "llm_service_types.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

char *response_to_json(const llm_response_t *resp);
llm_response_t *response_from_json(const char *json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_RESPONSE_H */
