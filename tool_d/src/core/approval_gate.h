// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file approval_gate.h
 * @brief core 域审批窄接口 —— 机制与策略分离之门。
 *
 * executor 只认识这个 opaque gate；真实审批策略（静态 ACL、SafetyGuard
 * 链、交互式审批）由 approval/ 域实现并注入。core/ 禁止反向 include
 * approval/ 的具体类型头（0.1.18 §12.16.4 强耦合处置条款）。
 *
 * fail-closed 语义：gate 未注入（NULL）时 executor 拒绝一切工具执行。
 */

#ifndef AIRY_RT_TOOL_D_CORE_APPROVAL_GATE_H
#define AIRY_RT_TOOL_D_CORE_APPROVAL_GATE_H

#include "tool_service_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct approval_gate approval_gate_t;

typedef enum {
    APPROVAL_GATE_DENIED = 0,
    APPROVAL_GATE_ALLOWED = 1,
} approval_gate_verdict_t;

/* 生命周期（实现在 approval/gate.c；gate 拥有全部内部件） */
approval_gate_t *gate_create_default(void);
void gate_destroy(approval_gate_t *gate);

/* 审批裁决：静态 ACL -> SafetyGuard 链 -> 交互式审批（ALWAYS 晋级为
 * 持久 ACL 规则）。ALLOWED 放行；DENIED 时 *out_reason（可 NULL）为
 * gate 分配的原因串，调用方接管所有权。 */
approval_gate_verdict_t gate_ask(approval_gate_t *gate, const char *agent_id,
                                 const tool_metadata_t *meta,
                                 const char *params_json, char **out_reason);

/* 交互审批查询与透传（池预算与 RPC tool.approve 路径） */
bool gate_int_enabled(const approval_gate_t *gate);
uint64_t gate_int_budget_ms(const approval_gate_t *gate);
char *gate_int_pending(approval_gate_t *gate);
int gate_int_resolve(approval_gate_t *gate, const char *request_id,
                     const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_D_CORE_APPROVAL_GATE_H */
