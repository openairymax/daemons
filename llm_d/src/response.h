/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file response.h
 * @brief 响应序列化接口
 */

#ifndef AIRY_RT_LLM_RESPONSE_H
#define AIRY_RT_LLM_RESPONSE_H

#include "llm_service.h"

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