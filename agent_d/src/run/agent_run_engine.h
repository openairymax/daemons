/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_engine.h
 * @brief Agent run engine internal declarations (M1-1a 引擎下沉)。
 *
 * agent.run 进程内引擎：会话注册表 + 编排（spawn+invoke）+ 工具循环
 * （ReAct）由 gateway 迁入 agent_d，agent_d 成为 agent.run 单入口；
 * gateway 仅做协议翻译/转发（见 daemons/gateway_d 的转发改造）。
 *
 * 依赖调用面：
 *   - think_d.process      （GCCP 双思考）
 *   - llm_d.complete_stream（工具循环主模型，RS 分帧增量）
 *   - tool_d.execute_tool  （工具执行）
 *   - mem_d.write          （会话持久化）
 * 统一经 daemon_rpc_client（Unix socket JSON-RPC）调用，与 agent_d
 * 既有 sched_d register_agent 调用同一路径。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_ENGINE_H
#define AIRY_RT_DAEMON_AGENT_RUN_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "airy_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 会话注册表（原 gateway_biz_agent_session.c gw_active_* 迁移） ---- */

#define AGENT_RUN_SESSION_ID_LEN 64
/* 工具环轮次预算（C-14）：请求参数优先，缺省取策略面
 * （AIRY_AGENT_TOOL_ROUNDS），再缺省 8；上限 32 是机制硬界，策略不得突破。
 * 到点不是崩溃：已执行 trace 与中间正文完整移交，并返回 AIRY_EBUDGET +
 * resume_token 供客户端续跑。 */
#define AGENT_RUN_ROUNDS_DEFAULT 8
#define AGENT_RUN_ROUNDS_MAX 32
/* 续跑令牌上界：只装「原始诉求 + 已完成步骤摘要 + 剩余子目标」，不装全量
 * 历史；二次续跑必须显著小于首次请求体。 */
#define AGENT_RUN_RESUME_TOKEN_MAX 4096
#define AGENT_RUN_RESUME_STEP_MAX 12
#define AGENT_RUN_RESUME_TEXT_MAX 768
/* 工具连败熔断：同一次 run 内连败 K 次即终止循环止损；rc 取
 * TOOL_FUSE（终局失败，原因随 response / error 事件通道回传）。 */
#define AGENT_RUN_TOOL_FAIL_LIMIT 3
#define AGENT_RUN_RC_TOOL_FUSE 2
/* 同调用换路（C-14）：同一工具以完全相同参数连续失败达到该次数后，下一次
 * 相同调用由机制层直接拒绝（不计入熔断），工具结果告知模型必须换参数、
 * 换工具或询问用户；trace 以 switch_reason 标注换路原因。 */
#define AGENT_RUN_TOOL_SWITCH_REPEAT 2
/* S-2 收敛：编排分支（subagent spawn/invoke）失败同为终局失败，rc 取本值；
 * 具体原因（阶段 + 契约错误符号）随 response / error 事件通道回传。 */
#define AGENT_RUN_RC_SUBAGENT_FAIL 3
/* 轮次预算耗尽不再是内部 rc：统一用 AIRY_EBUDGET(-124)，与 RPC/事件里的
 * error_code 同一事实源；产物与 resume_token 照常移交。 */
/* 无输出终态（0.1.21 L-9）：rc=0 但终局正文为空。空串不是结果——持久化
 * 它会让会话历史出现「成功但空白」的轮次，客户端只能靠用户重发；按失败
 * 回传，使上层可重试或改写提示。 */
#define AGENT_RUN_RC_EMPTY 6
/* 流式 LLM 调用的双窗口：TIMEOUT_MS 是相邻两段数据之间的最大沉默时间
 * （每收一段即重置），TOTAL_MS 是整条流的硬上限。只有前者会把持续吐 token
 * 的长推理在 90s 处腰斩——那是「卡住后报错」的直接来源。
 *
 * 预算树（0.1.21 C-21）：agent 是 llm_d / think_d / tool_d 的调用方，
 * 三个预算一律从 airy_budget 取，不再写死字面量——写死会让调用方与服务端
 * 各自漂移，think 曾因此出现双方都是 120s、零余量的死局。 */
#define AGENT_RUN_LLM_TIMEOUT_MS airy_budget_ms(AIRY_BUDGET_SERVER)
#define AGENT_RUN_LLM_TOTAL_MS 1800000
#define AGENT_RUN_THINK_TIMEOUT_MS airy_budget_ms(AIRY_BUDGET_SERVER)
#define AGENT_RUN_TOOL_TIMEOUT_MS airy_budget_outer(airy_budget_ms(AIRY_BUDGET_TOOL))
/* 编排预算不是被调用方的预算：spawn 是线程创建，invoke 覆盖一次完整子 run，
 * 只需保证不低于 server 层，否则子 run 会被父级提前掐断。 */
#define AGENT_RUN_SPAWN_TIMEOUT_MS 90000
#define AGENT_RUN_INVOKE_TIMEOUT_MS 180000
#define AGENT_RUN_AGENT_FILE_MAX (64 * 1024)

typedef struct agent_run_session_s {
    char session_id[AGENT_RUN_SESSION_ID_LEN];
    /* 运行标识（§2.4.2 信封 run_id）：每次 agent.run 独立生成，跨重连/多
     * 请求辨识同一次运行；与 session_id（会话槽位）解耦。 */
    char run_id[AGENT_RUN_SESSION_ID_LEN];
    volatile int cancelled;
    struct agent_run_session_s *next;
} agent_run_session_t;

/** @brief 查询会话是否已被取消。 */
bool agent_run_is_cancelled(const agent_run_session_t *s);

/** @brief 按 session_id 请求取消（agent.cancel 入口，返回 0 命中）。 */
int agent_run_cancel_by_session(const char *session_id);

/** @brief 终态名：run_end 的 status 与决策链的 state 共用这一映射点。 */
const char *run_state_name(int rc, int cancelled);

/** @brief rc 对应的用户可展示文案（run_end 错误帧与失败终局共用）。 */
const char *run_rc_msg(int rc);

/* ---- run 引擎主入口（原 gateway handle_agent_run 迁移） ---- */

/**
 * @brief run_stream 事件推送 sink（M1-1d 协议先行）。
 *
 * 引擎在 run_start / plan / tool_start / tool_end / token_delta /
 * message / run_end / error 节点调用 emit，把结构化事件信封推送出去。
 * emit 回调不得阻塞（写 socket 失败即丢弃）；ud 为回调上下文。
 */
typedef struct {
    void (*emit)(const char *type, cJSON *data, void *ud);
    void *ud;
} agent_run_event_sink_t;

/**
 * @brief 执行一次 agent.run（同步阻塞，事件驱动并发客户端下每请求一线程）。
 *
 * @param prompt       用户输入（非空）
 * @param model        模型名（可 NULL，用默认）
 * @param history      OpenAI messages 数组（可 NULL；非空时作为多轮上下文）
 * @param gccp_answers GCCP 两段式第二段答案 JSON（可 NULL）
 * @param agent_spec   params.agent 对象（可 NULL；存在时走编排分支）
 * @param agent_file   params.agent_file 路径（可 NULL；回退构建 spec）
 * @param session_id   调用方预分配会话 ID（可 NULL；为空则引擎生成）
 * @param rounds       工具环轮次预算（<=0 取策略缺省；上限 32）
 * @param resume_token 续跑令牌（可 NULL；非空时忽略 prompt/history 的全量重放）
 * @param sink         run_stream 事件推送 sink（可 NULL 表示非流式）
 * @param out_result   JSON-RPC result 对象（AIRY_* 成功时非 NULL，调用方
 *                     cJSON_Delete；GCCP 交互轮含 interaction_required 等）
 * @return 0 成功；1 用户取消；2 工具连败熔断（AGENT_RUN_RC_TOOL_FUSE）；
 *         3 编排失败（AGENT_RUN_RC_SUBAGENT_FAIL）；AIRY_EBUDGET 预算耗尽
 *         （产物与 resume_token 随 result 移交）；其余非零失败
 *         （失败时 *out_result 为 NULL）
 */
int agent_run_execute(const char *prompt, const char *model, const cJSON *history,
                      const char *gccp_answers, const cJSON *agent_spec,
                      const char *agent_file, const char *session_id, int rounds,
                      const char *resume_token, const agent_run_event_sink_t *sink,
                      cJSON **out_result);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_ENGINE_H */
