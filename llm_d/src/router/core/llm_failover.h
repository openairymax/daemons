/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_failover.h
 * @brief 端点切换决策件：错误码 → 可否换端点（纯函数、无状态）。
 *
 * 依据 0.1.19 架构改进方案 §3.8.2。切换从调用侧散落的 if 判断收敛为
 * 一处确定性映射，complete / stream 两入口共用同一判据。
 *
 * 判据与 §3.7 ErrorModel class 对齐，但有四处必须显式前置，因为 class
 * 粒度不足以区分「值得换」与「换了也白换」：
 *   - AIRY_ERR_IO 未进 err_class 显式表（默认落 business），须显式判 transport；
 *   - AIRY_ERR_LLM_AUTH_FAIL 归 config，但换端点可能绕开坏密钥，判 auth；
 *   - AIRY_ERR_LLM_RATE_LIMIT 归 transport，但需单独成类以便调用侧退避；
 *   - AIRY_ERR_SVC_NOT_READY 归 transport，但"端点未就绪"（自托管推理
 *     冷加载、上游重启）不是端点损坏，须独立成类以免污染健康账。
 *
 * business / policy 永不切：请求本身非法或策略拒绝，换端点必然同样失败。
 * 注意本判据回答「能否换端点」，与 providers 域「同一端点是否重发」
 * （provider_retry_class_t）是两个正交问题，不可互相替代。
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_FAILOVER_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_FAILOVER_H

#include "airy_abi.h"

AIRY_ABI_BEGIN

/** 切换原因分类。NONE 表示当前错误不属于任何可切换类别。 */
typedef enum {
    LLM_FO_NONE = 0,
    LLM_FO_TRANSPORT = 1,
    LLM_FO_RATE_LIMIT = 2,
    LLM_FO_AUTH = 3,
    LLM_FO_NOT_READY = 4
} llm_fo_reason_t;

/** 原因 → 白名单位。配置面 `on:` 列表按此掩码聚合。 */
#define LLM_FO_BIT(r) (1U << (unsigned)(r))

/** 缺省白名单：与 §3.8.2 `on: [transport, rate_limit, auth, not_ready]`
 * 同义。not_ready 与 transport 同样允许换端点，差别只在健康记账。 */
#define LLM_FO_ON_DEFAULT                                                                    \
    (LLM_FO_BIT(LLM_FO_TRANSPORT) | LLM_FO_BIT(LLM_FO_RATE_LIMIT) | LLM_FO_BIT(LLM_FO_AUTH) | \
     LLM_FO_BIT(LLM_FO_NOT_READY))

/**
 * 错误码 → 切换原因。
 * @param err_code 失败返回码，正负号均可（内部取绝对值归一）。
 * @return 可切换类别；不可切换或 err_code 为 0 时返回 LLM_FO_NONE。
 */
llm_fo_reason_t llm_fo_reason(int err_code);

/**
 * 按白名单判定是否切换。
 * @param err_code 失败返回码。
 * @param on_mask  LLM_FO_BIT() 聚合出的白名单位（见 LLM_FO_ON_DEFAULT）。
 * @return 1 可切换，0 不切换。
 */
int llm_fo_should(int err_code, unsigned on_mask);

/** 原因 → 稳定字符串名（"none"/"transport"/"rate_limit"/"auth"/"not_ready"）。 */
const char *llm_fo_name(llm_fo_reason_t reason);

/** 字符串名 → 原因；未知名称返回 LLM_FO_NONE，由调用侧决定告警策略。 */
llm_fo_reason_t llm_fo_parse(const char *name);

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_FAILOVER_H */
