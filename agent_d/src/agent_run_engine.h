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
 *   - llm_d.complete       （工具循环主模型）
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

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 会话注册表（原 gateway_biz_agent_session.c gw_active_* 迁移） ---- */

#define AGENT_RUN_SESSION_ID_LEN 64
#define AGENT_RUN_MAX_TOOL_LOOPS 8
#define AGENT_RUN_LLM_TIMEOUT_MS 90000
#define AGENT_RUN_THINK_TIMEOUT_MS 120000
#define AGENT_RUN_TOOL_TIMEOUT_MS 90000
#define AGENT_RUN_SPAWN_TIMEOUT_MS 90000
#define AGENT_RUN_INVOKE_TIMEOUT_MS 180000
#define AGENT_RUN_AGENT_FILE_MAX (64 * 1024)

typedef struct agent_run_session_s {
    char session_id[AGENT_RUN_SESSION_ID_LEN];
    volatile int cancelled;
    struct agent_run_session_s *next;
} agent_run_session_t;

/**
 * @brief 注册一个 in-flight run 会话（session_id -> cancelled=0）。
 * 引擎线程执行期间持有；agent.cancel(session_id) 置位后，工具循环
 * 轮间检查取消标志。
 */
agent_run_session_t *agent_run_register(const char *session_id);

/** @brief 注销会话并释放。 */
void agent_run_unregister(agent_run_session_t *s);

/** @brief 查询会话是否已被取消。 */
bool agent_run_is_cancelled(const agent_run_session_t *s);

/** @brief 按 session_id 请求取消（agent.cancel 入口，返回 0 命中）。 */
int agent_run_cancel_by_session(const char *session_id);

/**
 * @brief 生成唯一会话 ID（"sess_<hex>_<seq>"，与旧 gateway 格式一致，
 * 保证 agent.cancel 的 sess_ 前缀兼容）。
 */
void agent_run_gen_session_id(char *out, size_t out_size);

/* ---- run 引擎主入口（原 gateway handle_agent_run 迁移） ---- */

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
 * @param out_result   JSON-RPC result 对象（AIRY_* 成功时非 NULL，调用方
 *                     cJSON_Delete；GCCP 交互轮含 interaction_required 等）
 * @return 0 成功；1 用户取消；非零失败（*out_result 为 NULL）
 */
int agent_run_execute(const char *prompt, const char *model, const cJSON *history,
                      const char *gccp_answers, const cJSON *agent_spec,
                      const char *agent_file, const char *session_id, cJSON **out_result);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_ENGINE_H */
