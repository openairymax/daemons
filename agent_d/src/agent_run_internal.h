/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_internal.h
 * @brief Agent run engine 内部共享声明（模块私有，不对外导出）。
 *
 * agent_run_engine.c（会话注册表 + spec 解析 + 编排 + 主入口）与
 * agent_run_loop.c（工具循环 ReAct）之间的共享契约；RPC 适配层只依赖
 * agent_run_engine.h 的公共接口。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "agent_run_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 工具循环域（agent_run_loop.c） ---- */

/**
 * @brief 组装 §2.4 v1 事件信封 JSON 并交给 sink->emit（agent_run_engine.c）。
 * data 所有权转移给本函数；emit 不得阻塞。sink 类型见 agent_run_engine.h。
 */
void agent_run_emit_event(const agent_run_event_sink_t *sink, uint64_t *seq,
                          const char *session_id, const char *type, cJSON *data);

/**
 * @brief ReAct 工具循环：LLM complete -> tool_calls -> tool_d 执行 -> 回填。
 *
 * @param prompt    用户输入（history 为空时构造首条消息）
 * @param history   OpenAI messages 数组（可 NULL）
 * @param model     模型名（非 NULL）
 * @param session   会话（轮间取消检查；可 NULL 表示无取消能力）
 * @param sink      run_stream 事件推送 sink（可 NULL 表示非流式）
 * @param out_trace 工具 trace 数组（成功时非 NULL，调用方 cJSON_Delete）
 * @param out_text  最终回复文本（AIRY_* 分配，调用方 AIRY_FREE）
 * @param out_tokens 累计 token
 * @param out_cost  累计成本
 * @param out_reasoning 累计思考链（可 NULL）
 * @return 0 成功；1 用户取消；非零失败
 */
int agent_run_tool_loop(const char *prompt, const cJSON *history, const char *model,
                        const agent_run_session_t *session, const agent_run_event_sink_t *sink,
                        cJSON **out_trace, char **out_text, uint64_t *out_tokens, double *out_cost,
                        char **out_reasoning);

/* ---- 编排域（agent_run_engine.c） ---- */

/**
 * @brief 编排分支：进程内 spawn+invoke（agent.run 单入口，无 RPC 环）。
 *
 * @param agent_spec params.agent 对象（非 NULL）
 * @param prompt     invoke 输入
 * @param out_text   输出文本（AIRY_* 分配，调用方 AIRY_FREE）
 * @param out_err    失败原因（AIRY_* 分配；成功为 NULL）
 * @return 0 成功；非零失败
 */
int agent_run_orchestrate(const cJSON *agent_spec, const char *prompt, char **out_text,
                          char **out_err);

/**
 * @brief 从 params.agent_file 解析 agent spec（JSON / 简单 YAML role）。
 * @return 新分配 cJSON 对象（调用方 cJSON_Delete）；无有效 spec 返回 NULL
 */
cJSON *agent_run_spec_from_file(const cJSON *params);

/**
 * @brief 会话持久化：成功后把本轮问答写入 mem_d（best effort）。
 */
void agent_run_persist(const char *session_id, const char *user_prompt,
                       const char *assistant_text);

/**
 * @brief 记录一条 hall 事件（会话决策链写侧，经 daemon_hall_write）。
 */
void agent_run_record_event(const char *session_id, const char *category, cJSON *content);

/**
 * @brief 调用 think_d.process（GCCP 双思考；降级容忍）。
 * @return 0 成功（*out_think 有效，调用方 cJSON_Delete）；非零失败
 */
int agent_run_think_process(const char *session_id, const char *prompt,
                            const char *gccp_answers, cJSON **out_think);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H */
