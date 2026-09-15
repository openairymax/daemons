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
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 上下文台账域（agent_run_ledger.c） ---- */

#define AGENT_LEDGER_ID_LEN 33   /* mem_d entry_id：32 hex + NUL（ledger.h） */
#define AGENT_LEDGER_TYPE_LEN 12 /* 入口类型字符串上界（含 NUL） */

/** 台账条目 ↔ 消息映射（entry_id 为空表示已压缩的摘要占位）。 */
typedef struct {
    char entry_id[AGENT_LEDGER_ID_LEN];
    char type[AGENT_LEDGER_TYPE_LEN];
    cJSON *msg; /* messages 数组内节点引用（非拥有） */
} agent_ledger_item_t;

/** 会话台账客户端状态：mem_d socket + 记账映射（渐进降级，disabled 时不记账）。 */
typedef struct {
    int enabled;
    char sock[AIRY_PATH_MAX];
    char sess[AGENT_RUN_SESSION_ID_LEN];
    agent_ledger_item_t *items;
    size_t count;
    size_t cap;
} agent_ledger_t;

/**
 * @brief 初始化台账客户端（sess 为空或 socket 解析失败时置 disabled）。
 * @return 恒为 0（enabled 状态经结构体表达）
 */
int agent_ledger_init(agent_ledger_t *lg, const char *sess);

/** @brief 释放映射数组（不拥有 messages 节点，无需释放引用）。 */
void agent_ledger_free(agent_ledger_t *lg);

/**
 * @brief 记账一条消息：ledger_append 落库并建立 entry_id ↔ msg 映射。
 * 失败静默（渐进降级），不阻断主对话。
 */
void agent_ledger_add(agent_ledger_t *lg, const char *type, const char *text, size_t token_out,
                      cJSON *msg);

/**
 * @brief 每轮 LLM 调用前预算适配：ledger_window warn 时对候选区
 * （ReAct 轮次成组，入口 user 与末组受保护）执行 mem.compress，
 * 用压缩上下文原位替换本地消息组。
 */
void agent_ledger_fit(agent_ledger_t *lg, cJSON *messages);

/* ---- 工具循环域（agent_run_loop.c） ---- */

/**
 * @brief 组装 §2.4 v1 事件信封 JSON 并交给 sink->emit（agent_run_engine.c）。
 * data 所有权转移给本函数；emit 不得阻塞。sink 类型见 agent_run_engine.h。
 * run_id 为本次运行标识（§2.4.2 信封字段，每帧携带，可 NULL）。
 */
void agent_run_emit_event(const agent_run_event_sink_t *sink, uint64_t *seq,
                          const char *run_id, const char *session_id, const char *type,
                          cJSON *data);

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
