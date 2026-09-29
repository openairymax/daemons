// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file metrics.c
 * @brief Input-complexity evaluation and routing-decision audit logs.
 */

#include "metrics.h"
#include "svc_logger.h"

#include <string.h>

/**
 * @brief Assess complexity from the input text (SIMPLE/MODERATE/COMPLEX)
 *
 * Scoring rules:
 *   - input length > 500 chars +2
 *   - input length > 100 chars +1
 *   - contains architecture/design/system-level keywords +1
 *   - contains multi-step markers +2
 *   - contains code-generation markers +1
 *
 * Routing:
 *   - SIMPLE   (0-1):  gpt-4o-mini
 *   - MODERATE (2-4):  gpt-4o
 *   - COMPLEX  (5+):   claude-sonnet
 */
llm_complexity_level_t assess_complexity(const char *input)
{
    if (!input)
        return LLM_COMPLEXITY_SIMPLE;

    size_t len = strlen(input);
    int score = 0;

    if (len > 500)
        score += 2;
    else if (len > 100)
        score += 1;

    const char *complex_kw[] = {"architecture", "distributed", "system design", "scalability",
                                "架构",         "分布式",      "系统设计",      "高可用",
                                "微服务",       "重构"};
    for (size_t i = 0; i < sizeof(complex_kw) / sizeof(complex_kw[0]); i++) {
        if (strstr(input, complex_kw[i])) {
            score += 1;
            break;
        }
    }

    const char *multi_step_kw[] = {"first", "then", "finally", "step 1", "step 2",
                                   "首先",  "然后", "最后",    "第一步", "第二步"};
    for (size_t i = 0; i < sizeof(multi_step_kw) / sizeof(multi_step_kw[0]); i++) {
        if (strstr(input, multi_step_kw[i])) {
            score += 2;
            break;
        }
    }

    const char *code_kw[] = {"function", "algorithm", "implement", "write a", "函数",
                             "算法",     "实现",      "编写",      "代码"};
    for (size_t i = 0; i < sizeof(code_kw) / sizeof(code_kw[0]); i++) {
        if (strstr(input, code_kw[i])) {
            score += 1;
            break;
        }
    }

    if (score >= 5)
        return LLM_COMPLEXITY_COMPLEX;
    if (score >= 2)
        return LLM_COMPLEXITY_MODERATE;
    return LLM_COMPLEXITY_SIMPLE;
}

/**
 * @brief Record the routing-decision audit log
 */
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason)
{
    const char *complexity_names[] = {"SIMPLE", "MODERATE", "COMPLEX"};
    SVC_LOG_INFO("[ROUTING] model=%s complexity=%s input_len=%zu reason=%s",
                 model ? model : "unknown", complexity_names[complexity], input_len,
                 reason ? reason : "default");
}
