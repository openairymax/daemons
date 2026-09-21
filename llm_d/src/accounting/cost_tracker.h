/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cost_tracker.h
 * @brief Cost-tracking interface (configurable pricing).
 */

#ifndef AIRY_RT_LLM_COST_TRACKER_H
#define AIRY_RT_LLM_COST_TRACKER_H

#include <cjson/cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *model_pattern;
    double input_price_per_k;
    double output_price_per_k;
} pricing_rule_t;

typedef struct cost_tracker cost_tracker_t;

cost_tracker_t *cost_tracker_create(const pricing_rule_t *rules, int rule_count);
void cost_tracker_destroy(cost_tracker_t *ct);
void cost_tracker_add(cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                      uint32_t completion_tokens);

double cost_tracker_estimate(const cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                             uint32_t completion_tokens);
cJSON *cost_tracker_export(cost_tracker_t *ct);

/* 2.1.1.5 修复：计费/用量持久化——daemon 重启后累计金额与 token 历史
 * 不再清零。save 以 JSON 原子写（临时文件 + rename）落盘；
 * load 从文件读取并合并进当前累计（幂等，可多次调用）。 */
int cost_tracker_save(cost_tracker_t *ct, const char *path);
int cost_tracker_load(cost_tracker_t *ct, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_COST_TRACKER_H */