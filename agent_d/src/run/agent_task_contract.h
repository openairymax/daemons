/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_task_contract.h
 * @brief 统一任务编排入口契约（0.1.21 C-15）。
 *
 * `task.submit` 是 CLI / TUI / SDK 的唯一任务入口：客户端只提交目标，
 * 由服务端裁决 chat/task、生成 task_id、决定是否进入计划执行；客户端
 * 不得再手持 `[MODE:TASK]` 之类的正文标记。
 *
 * 本头只承载 wire 常量与契约声明，实现见 agent_task_orchestrate.c：
 *   - mode 裁决（规则优先、模型兜底）与 mode_reason；
 *   - goal 完备性（gccp_confirmed / gccp_missing）与 needs_intent 门禁；
 *   - task_id / plan_id 生成（事件信封 run_id 的权威来源）。
 */

#ifndef AIRY_RT_DAEMON_AGENT_TASK_CONTRACT_H
#define AIRY_RT_DAEMON_AGENT_TASK_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 标识上界 ---- */

#define AGENT_TASK_ID_LEN 64        /* task-<ts>-<n> 上界（含 NUL） */
#define AGENT_TASK_PLAN_ID_LEN 64   /* plan-<ts>-<n> 上界（含 NUL） */
#define AGENT_TASK_REASON_LEN 64    /* mode_reason 上界（含 NUL） */
#define AGENT_TASK_MISSING_MAX 8    /* gccp_missing 槽位上限 */
#define AGENT_TASK_MISSING_KEY_LEN 32 /* 单个缺失槽位名上界（含 NUL） */

/* 缺省轮次预算沿用 C-14 的机制硬界 [1,32]；本入口只做夹取不另立上限。 */
#define AGENT_TASK_ROUNDS_MAX 32

/* 规则裁决阈值：长目标按字符（码点）计，与 §16.14.3 的 128 字基线一致。 */
#define AGENT_TASK_LONG_GOAL_CHARS 128
#define AGENT_TASK_SHORT_CHAT_CHARS 6

/* ---- mode（wire 值） ---- */

#define AGENT_TASK_MODE_AUTO "auto"
#define AGENT_TASK_MODE_CHAT "chat"
#define AGENT_TASK_MODE_TASK "task"

/* ---- mode_reason（wire 值；规则命中顺序即这里的书写顺序） ---- */

#define AGENT_TASK_REASON_EXPLICIT_CHAT "explicit:client_declared_chat"
#define AGENT_TASK_REASON_EXPLICIT_TASK "explicit:client_declared_task"
#define AGENT_TASK_REASON_LONG_GOAL "rule:long_goal"
#define AGENT_TASK_REASON_RESOURCE_VERB "rule:resource_operation_verb"
#define AGENT_TASK_REASON_ENGINEERING_VERB "rule:engineering_verb"
#define AGENT_TASK_REASON_SHORT_CHAT "rule:short_conversational_goal"
#define AGENT_TASK_REASON_DEFAULT_CHAT "default_chat"

/* ---- result.status（task.submit 一级终态） ---- */

#define AGENT_TASK_STATUS_OK "ok"
#define AGENT_TASK_STATUS_NEEDS_INTENT "needs_intent"
/* C-17：显式声明 task 却未给出任何可执行判据——拒绝进入并回示例，
 * 客户端补齐 success_criteria 后重提。 */
#define AGENT_TASK_STATUS_NEEDS_CRITERIA "needs_criteria"

/* ---- task_state 事件的 state 取值 ---- */

#define AGENT_TASK_STATE_SUBMITTED "submitted"
#define AGENT_TASK_STATE_PLANNED "planned"
#define AGENT_TASK_STATE_EXECUTING "executing"
#define AGENT_TASK_STATE_COMPLETED "completed"
#define AGENT_TASK_STATE_PAUSED "paused"
#define AGENT_TASK_STATE_FAILED "failed"
#define AGENT_TASK_STATE_CANCELLED "cancelled"
/* C-17 / D-13：判据无法取证——内容可交付但明确不是已完成，属终态之一
 * （看板与回放必须与 completed 区分，不能显示成功）。 */
#define AGENT_TASK_STATE_UNVERIFIED "unverified"

/* task.submit_stream 的 SSE 请求体方法名（gateway 白名单既有 agent.run_stream） */
#define AGENT_TASK_METHOD_SUBMIT "task.submit"
#define AGENT_TASK_METHOD_SUBMIT_STREAM "task.submit_stream"

/** 缺失的 goal 槽位名（gccp_missing 数组元素）。 */
typedef char agent_task_missing_slot_t[AGENT_TASK_MISSING_KEY_LEN];

/**
 * @brief 一次 task 提交的元数据（由 RPC 层持有，引擎仅借用）。
 *
 * task_id/plan_id 在提交时生成并全程只读；事件信封的 run_id 一律取
 * task_id，使 CLI 与 TUI 对同一任务看到同一条时间线（N3 修复）。
 */
typedef struct {
    char task_id[AGENT_TASK_ID_LEN];   /* 服务端生成：task-<ts>-<n> */
    char plan_id[AGENT_TASK_PLAN_ID_LEN];
    char mode[8];                      /* chat / task（裁决结果） */
    char mode_reason[AGENT_TASK_REASON_LEN];
    bool gccp_confirmed;               /* goal 是否已确认；false 不得执行工具 */
    bool allow_dangerous;              /* 调用方显式接受高危工具风险（审计留痕） */
    const char *success_criteria;      /* 可 NULL；计划/验收的判定依据 */
    const char *client;                /* cli / tui / sdk（审计用，可 NULL） */
    int budget_rounds;                 /* 已夹取到 [1,32] */
    long budget_tokens;                /* 0 = 不设预算 */
    long budget_seconds;               /* 0 = 不设预算 */
} agent_run_task_meta_t;

/**
 * @brief 裁决 chat / task（规则优先；declared_mode 为显式声明）。
 *
 * 命中顺序：显式声明 -> 长目标 -> 资源操作动词 -> 工程动词 ->
 * 短对话 -> 缺省 chat。declared_mode 为 "chat"/"task" 时直接返回
 * explicit:* 原因，不受规则覆盖。
 *
 * @param goal         用户目标（非 NULL）
 * @param declared_mode 客户端声明（可 NULL / "auto"）
 * @param mode         输出："chat" 或 "task"
 * @param mode_size    mode 缓冲长度
 * @param reason       输出：命中原因（wire 值）
 * @param reason_size  reason 缓冲长度
 * @return 0 成功；负值参数非法
 */
int atk_decide_mode(const char *goal, const char *declared_mode, char *mode, size_t mode_size,
                           char *reason, size_t reason_size);

/**
 * @brief 判定 goal 完备性（GCCP 确认门禁）。
 *
 * 无 gccp_answers 时 confirmed 保持调用方声明值（缺省 false），missing
 * 为空——参考行为：`打开火狐` 返回 needs_intent 且 missing=[]；有答案
 * 时逐槽校验 goal/context/constraints/plan，任一缺失即 confirmed=false。
 *
 * @param gccp_answers  GCCP 答案 JSON 字符串（可 NULL）
 * @param confirmed     [in,out] 进入为客户端声明值，返回为最终确认态
 * @param missing       输出：缺失槽位名数组（调用方分配，容量 >= max_missing）
 * @param max_missing   missing 容量
 * @param missing_count 输出：缺失槽位数
 * @return 0 成功；负值解析失败（按未确认处理）
 */
int gccp_ok(const cJSON *gccp_answers, bool *confirmed,
                          agent_task_missing_slot_t *missing, size_t max_missing,
                          size_t *missing_count);

/** @brief 生成 `<prefix>-<秒级时间戳>-<进程内序号>`（prefix 非 NULL）。 */
void agent_task_gen_id(const char *prefix, char *out, size_t size);

/**
 * @brief 执行门禁（唯一实现）：未确认的 task 不得触达引擎与工具。
 *
 * 门禁条件只有一条——mode=task 且 gccp_confirmed=false 时关闭；chat
 * 与已确认任务一律放行。关闭时调用方必须在调用引擎之前返回
 * needs_intent（零引擎 / 零工具调用，gate:lab 判据）。
 *
 * @param mode           裁决结果（可 NULL，按 chat 处理）
 * @param gccp_confirmed 用户是否已确认目标/计划
 * @return true 放行；false 需返回 needs_intent
 */
bool gate_allows(const char *mode, bool gccp_confirmed);

/**
 * @brief 组装 needs_intent 结果（confirmed=false 的唯一出口）。
 *
 * 返回对象含 status/mode/mode_reason/gccp_confirmed/gccp_missing 与
 * 别名 missing（兼容 285H 参考输出）；不执行任何工具。
 */
cJSON *need_intent(const agent_run_task_meta_t *meta,
                                     agent_task_missing_slot_t *missing, size_t missing_count);

/* ---- 完成判据（C-17）：校验、计划派生、needs_criteria 出口 ---- */

/* 单次复核的判据条数上界，与 tool_d verifier 的 AIRY_VERIFY_MAX_ITEMS 一致；
 * 只在契约层夹取，超限即视为非法（fail-closed）。 */
#define AGENT_TASK_CRITERIA_MAX 16

/**
 * @brief 判据是否可用：数组（或 {"criteria":[...]}）且每条 1..MAX 项合法。
 *
 * 合法性 = type ∈ {cmd,file,service,rpc} 且该类型必需字段齐全（cmd.cmd /
 * file.path / service.port / rpc.daemon+method）。语义判定在 tool_d，此处
 * 只做「能否交给 verifier」的静态门禁。
 */
bool criteria_ok(const char *criteria);

/**
 * @brief 由计划节点汇总生成判据串（用户未给判据时的唯一派生路径）。
 *
 * 读取 plan.steps[].postcondition（对象/数组形态，字段同 success_criteria
 * 条目）；不含显式 type 的文本型 postcondition 一律忽略——不得把自然语言
 * 说明猜成 shell 命令，否则会制造假失败。
 *
 * @param plan think_d 计划对象
 * @return 判据 JSON 数组串（AIRY_MALLOC，调用方释放）；无可派生项返回 NULL
 */
char *criteria_from_plan(const cJSON *plan);

/** @brief 组装 needs_criteria 结果（拒绝进入 task 的唯一出口，含补判据示例）。 */
cJSON *need_criteria_of(const agent_run_task_meta_t *meta, const char *why);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_TASK_CONTRACT_H */
