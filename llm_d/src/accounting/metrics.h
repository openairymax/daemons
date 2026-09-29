/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file metrics.h
 * @brief Input-complexity evaluation and routing-decision audit
 *        interface (accounting domain).
 */

#ifndef AIRY_RT_LLM_METRICS_H
#define AIRY_RT_LLM_METRICS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Complexity assessment levels
 */
typedef enum {
    LLM_COMPLEXITY_SIMPLE = 0,
    LLM_COMPLEXITY_MODERATE = 1,
    LLM_COMPLEXITY_COMPLEX = 2
} llm_complexity_level_t;

llm_complexity_level_t assess_complexity(const char *input);
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_METRICS_H */
