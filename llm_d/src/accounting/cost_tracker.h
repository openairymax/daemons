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

/* 价格查找 SSoT：累计/估算与端点路由表共用；首条命中胜出，pattern 为精确名或
 * 尾部 '*'；未命中取内置默认价，故默认价只有一处。model 为 NULL 即默认价。 */
void cost_tracker_price(const pricing_rule_t *rules, int rule_count, const char *model,
                        double *input_price, double *output_price);
void cost_tracker_add(cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                      uint32_t completion_tokens);

double cost_tracker_estimate(const cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                             uint32_t completion_tokens);
cJSON *cost_tracker_export(cost_tracker_t *ct);

/* 计费/用量持久化：save 以 JSON 原子写（临时文件 + rename）落盘；
 * load 从文件读取并合并进当前累计（幂等，可多次调用）。 */
int cost_tracker_save(cost_tracker_t *ct, const char *path);
int cost_tracker_load(cost_tracker_t *ct, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_COST_TRACKER_H */
