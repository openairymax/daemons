/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_review.h
 * @brief 执行中语义复核出口：把 run 产物交 think_d 做整链复核并取回判定。
 *
 * 机制与策略分离：复核管线（确定性门禁 → t2 → t1-f）与降级链在 coreloopthree
 * （execution_review），由 think_d 在本进程内驱动；语义判断策略与提示词同样在
 * think_d（think.review）。本文件只声明执行体侧的挂载点：agent_d 决定「何时
 * 复核、判定如何收尾」，不链接机制库、不持有提示词；think_d 不可达或未配置
 * 模型时返回 SKIP，调用方保持既有结果。
 */

#ifndef AGENT_REVIEW_H
#define AGENT_REVIEW_H

#include "airy_rt.h"

#include <stddef.h>

/* 归因文案缓冲：与 agent_verify 的 reason 同量级，足够放下模型给的一句话。 */
#define AGENT_REVIEW_REASON 192

typedef struct {
    int verdict; /* airy_review_verdict_t：PASS/DRIFT/REJECT/SKIP */
    char reason[AGENT_REVIEW_REASON];
} agent_review_result_t;

/**
 * @brief 对一次 run 的产物做整链语义复核。
 * @param goal 蓝图/任务目标（借用，可 NULL）
 * @param body 产物正文（借用，必填；空正文由门禁直接拒绝）
 * @param artifact_json 产物结构 JSON 串（借用，可 NULL，缺省用正文）
 * @param facts_json tool_d 复核证据 JSON 串（借用，可 NULL）
 * @param out 判定输出（必填）
 * @return 0 = 已产出判定（含 SKIP 降级）；-1 = 无法复核（调用方保持既有结果）
 */
int agent_rev_apply(const char *goal, const char *body, const char *artifact_json,
                    const char *facts_json, agent_review_result_t *out);

#endif /* AGENT_REVIEW_H */
