/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_verify.h
 * @brief C-17 完成判据外部化：agent_d 侧唯一复核入口。
 *
 * 判据由 tool_d verifier 在独立上下文只读执行（tool.verify），agent_d 只
 * 负责搬运、重试与归因，不解释判据语义——机制与策略分离：agent_d 决定
 * 「何时复核、失败如何收尾」，tool_d 决定「如何取证」。
 */

#ifndef AIRY_RT_DAEMON_AGENT_VERIFY_H
#define AIRY_RT_DAEMON_AGENT_VERIFY_H

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 判据复核的传输重试：只对「verifier 不可达」重试，判据未过是确定性结果，
 * 重试同一判据不会改变答案。 */
#define AGENT_VERIFY_RETRY 3
#define AGENT_VERIFY_RETRY_MS 300

/**
 * @brief 对 success_criteria 执行只读取证复核。
 *
 * @param criteria        判据 JSON（数组，或 {"criteria":[...]} 对象；字符串形式）
 * @param out_verification 复核结果对象（verifier 原始载荷：ok/total/passed/
 *                         items[{type,expect,actual,pass}]），调用方
 *                         cJSON_Delete；verifier 不可达时为 NULL
 * @return 0 全部通过（或 on_fail=continue 放行）；
 *         AGENT_RUN_RC_UNMET 判据未过（*out_verification 含逐项证据）；
 *         AGENT_RUN_RC_UNVERIFIED verifier 连续不可达，未能取证。
 */
int agent_verify_apply(const char *criteria, cJSON **out_verification);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_VERIFY_H */
