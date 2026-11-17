/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_task_ledger.h
 * @brief 任务事件账本的唯一写入面（0.1.21 C-18）。
 *
 * 工作大厅的判据是「一次任务在 board 出现终态、replay(task_id) 全链
 * 含 progress/result/issue、失败必留事件」。此前 task 的决策链按
 * session_id 落盘，而 task_id/plan_id 只存在于事件信封里，回放必须
 * 先猜目录；失败与取消也只发流式帧、不落盘。
 *
 * 本模块收敛账本主键与四类事件的磁盘写入：
 *   - blueprint：提交时的计划快照（goal + 成功判据）；
 *   - progress：状态推进（submitted/planned/executing/paused）；
 *   - result：终态（completed/failed/cancelled）；
 *   - issue：失败/取消的字段级原因。
 *
 * 主键选择：task 提交（有 task 元数据）一律用 task_id，chat 会话回退
 * session_id——同一 id 贯穿信封、目录、看板与回放，四标识不再分叉。
 * 目录与文件格式由 commons/utils/hall/hall_event.c 单点定义。
 */

#ifndef AIRY_RT_DAEMON_AGENT_TASK_LEDGER_H
#define AIRY_RT_DAEMON_AGENT_TASK_LEDGER_H

#include "agent_task_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 账本主键：task_id 优先，无 task 元数据回退 session_id。
 * @return 主键指针（借用入参，不分配）；两者皆空时返回 NULL。
 */
const char *agent_task_ledger_key(const agent_run_task_meta_t *task, const char *session_id);

/**
 * @brief 写 blueprint 事件（任务提交时的计划快照）。
 * @return 0 成功；负值参数非法或写盘失败（best effort，调用方不阻塞主流程）。
 */
int agent_task_ledger_blueprint(const char *task_id, const char *plan_id, const char *goal,
                                const char *success_criteria);

/**
 * @brief 写 progress 事件（状态推进 + 0..1 进度）。
 * @param state  task_state 取值（submitted/planned/executing/paused）
 * @param reason 可 NULL（如 gccp_interaction_required）
 */
int agent_task_ledger_progress(const char *task_id, const char *plan_id, const char *state,
                               const char *reason);

/**
 * @brief 写 issue 事件（失败/取消原因；rc 与 reason 至少一者可判读）。
 */
int agent_task_ledger_issue(const char *task_id, int rc, const char *reason);

/**
 * @brief 写 result 事件（终态：completed/failed/cancelled）。
 */
int agent_task_ledger_result(const char *task_id, const char *plan_id, const char *state);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_TASK_LEDGER_H */
