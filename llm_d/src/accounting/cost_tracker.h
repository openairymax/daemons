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

/* 换价格表（热重载）：深拷贝后原子替换，调用方的数组可立即释放。
 * 计数累计不受影响。@return 0 成功；-1 参数或内存失败（旧表保持）。 */
int cost_set_rules(cost_tracker_t *ct, const pricing_rule_t *rules, int rule_count);

/* 价格查找 SSoT：累计/估算与端点路由表共用；首条命中胜出，pattern 为精确名或
 * 尾部 '*'；未命中取内置默认价，故默认价只有一处。model 为 NULL 即默认价。 */
void cost_tracker_price(const pricing_rule_t *rules, int rule_count, const char *model,
                        double *input_price, double *output_price);
/* cached_tokens 为 prompt 命中上游前缀缓存的明细累计（不改计价，仅观测）。 */
void cost_tracker_add(cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                      uint32_t completion_tokens, uint32_t cached_tokens);

double cost_tracker_estimate(const cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                             uint32_t completion_tokens);
cJSON *cost_tracker_export(cost_tracker_t *ct);

/* 计费/用量持久化：save 以 JSON 原子写（临时文件 + rename）落盘；
 * load 从文件读取并合并进当前累计（幂等，可多次调用）。 */
int cost_tracker_save(cost_tracker_t *ct, const char *path);
int cost_tracker_load(cost_tracker_t *ct, const char *path);

/* 聚合落盘（热路径入口）：dirty 且「累计 ≥64 次」或「距上次同步 ≥1s」才
 * 真正写盘，否则零 I/O 直接返回 0。窗口内的次数触发只刷页缓存（进程崩溃
 * 不丢），时间窗口到期才做强同步（断电丢失 ≤1s）；间隔检查由请求驱动，
 * 空闲期不产生唤醒。进程退出仍由 save 同步落盘。
 * @return 0 成功或无需落盘；-1 参数/写盘失败（保持 dirty 待重试）。 */
int cost_tracker_flush(cost_tracker_t *ct, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_COST_TRACKER_H */
