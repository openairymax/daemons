/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file think_service.h
 * @brief Public interface of the dual-think system (Thinkdual) service.
 *
 * think_d hosts the CoreLoopThree cognitive engine, integrating the
 * dual-think system into the daemon runtime:
 *   - t2 (slow-think, model A): planner + Phase-2 generation (GRAD s2)
 *   - t1-f (fast-think, model B): context arbiter (GRAD s1 final
 *     accept/reject call; daily-chat route)
 *   - t1-p (professional, model C): logic verifier — deterministic
 *     four-check (zero-token) gate in GRAD, not an LLM arbitration
 *
 * Dual-thinking model (2026-08-07 decision): GCCP fact lock (Phase 0
 * intent confirmation) + GRAD logic lock (Phase-1 plan-level critique
 * loop over the generated plan; the old text-level TC3 critique and the
 * dual_coordinate cross-validation were removed). GRAD loop: model A
 * (t2) drafts the plan -> model C (t1-p) runs the deterministic
 * verifier -> model B (t1-f) arbitrates -> converge or fall back to the
 * seed plan. Working-memory keys: "gccp_goal", "cog_review_decision".
 *
 * LLM calls go through llm_svc_adapter directly to the llm_d Unix socket
 * (daemon_rpc_call), interoperating natively with the daemon
 * architecture. End-cloud hybrid: both local endpoints (Ollama/vLLM via
 * the local/custom providers) and cloud APIs (deepseek/openai/anthropic
 * /google/glm/qwen/moonshot/siliconflow/spark/custom) are reached
 * through the same llm_d socket; the router applies policy-level
 * fallback (cost-aware -> round-robin chain).
 */

#ifndef AIRY_RT_THINK_SERVICE_H
#define AIRY_RT_THINK_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct think_service think_service_t;

/** @brief Dual-think service config. */
typedef struct {
    int enabled;
    const char *think2_slow_model;
    const char *think1_fast_model;
    const char *think1_prof_model;
    uint32_t process_timeout_ms;
    uint32_t max_feedback_events;
} think_service_config_t;

/**
 * @brief Thinking-process result (JSON string, caller AIRY_FREEs).
 *
 * Shaped like:
 * {
 *   "plan": {"task_plan_id","node_count","nodes":[{id,goal,handler,role,depends}]},
 *   "feedback": [{level,module,event,data}...],
 *   "stats": {"dual_thinking_enabled":1,"dual_invocations":N,"corrections":N}
 * }
 */
typedef struct {
    char *json;
    size_t json_len;
} think_process_result_t;


think_service_t *think_service_create(const think_service_config_t *config);
void think_service_destroy(think_service_t *svc);


/**
 * @brief Dual-think processing: input prompt -> cognitive engine (GCCP +
 *        GRAD) -> JSON result.
 *
 * GCCP 两段式交互（P-A, 2026-08-23；会话隔离加固 2026-08-25）：当 GCCP
 * 判定输入需要澄清时——
 *   第一段：gccp_answers 传 NULL。引擎挂起并返回问题集，结果 JSON 含
 *     {"gccp_need_interaction":1,"gccp_questions":[{id,question,hint,
 *     required}...]}（返回 0，客户端据此进入问答轮，不浪费后续 Phase token）。
 *   第二段：客户端收集答案后以 answers JSON（如 {"endpoint":"...",
 *     "start":"...","bottleneck":"...","audience":"..."}）重发，引擎据此
 *     完成目标确认并继续 GCCP+GRAD 完整链路（结果含 plan/feedback/stats）。
 *   用户放弃问答：直接省略重发即可（无副作用）。
 *
 * 会话隔离（2026-08-25 修复）：GCCP 交互状态（问题集/答案）按 session_id
 * 隔离存储，杜绝多客户端并发串台——此前为服务级单份状态，请求 B 的第一段
 * 会覆盖请求 A 的问题集，A 的第二段答案可能被 B 消费，触发"二次挂起"与
 * 重复提问。session_id 为 NULL/空时使用 "default" 兜底（单会话兼容）。
 *
 * @param svc Service handle
 * @param session_id 会话标识（可 NULL；GCCP 交互状态隔离维度）
 * @param prompt User input (UTF-8)
 * @param gccp_answers 用户答案 JSON（可 NULL；第二段携带）
 * @param out_result Output result (OWNER, caller frees via think_result_free)
 * @return 0 on success (含 gccp_need_interaction=1 的问答轮), non-zero on failure
 */
int think_service_process(think_service_t *svc, const char *session_id, const char *prompt,
                          const char *gccp_answers, think_process_result_t *out_result);

void think_result_free(think_process_result_t *res);


/**
 * @brief 流程编排执行（S-5 恢复的 orchestrator 管线：分解→规划→生成→
 *        批判→验证→审计→对齐，含熔断/重试/超时/进度回调）。
 *
 * 与 think.process（单次五阶段认知引擎）双管线并存：orchestrate 面向
 * 多任务/多阶段编排场景（子任务分发、自定义 pipeline）。
 *
 * @param svc Service handle
 * @param input 编排输入（自然语言任务）
 * @param timeout_ms 超时毫秒（0=使用编排器默认）
 * @param out_json 输出 JSON（OWNER，调用方 AIRY_FREE）：
 *        {"run_id","phases":[{phase,status,error_code,duration_ms,output}],"success"}
 * @return 0 成功；非 0 失败（管线中途失败时返回 0 且 success=false）
 */
int think_service_orchestrate(think_service_t *svc, const char *input, uint32_t timeout_ms,
                              char **out_json);

/**
 * @brief 注入 orchestrator ops 表（airy_orch_ops_t）。
 *
 * think_d 实现编排接口并注入 ops 注入框架；atoms 侧组件（如工作大厅）
 * 可经 are_ops_get_orch() 调度编排，无需链接 daemons。
 *
 * @param svc Service handle（create 成功后调用）
 */
void think_orch_ops_inject(think_service_t *svc);


/**
 * @brief Get dual-think statistics (JSON string, caller AIRY_FREEs).
 * @param svc Service handle
 * @return JSON string (with dual_invocations/corrections/llm_backed, etc.),
 *         NULL on failure
 */
char *think_service_stats_json(think_service_t *svc);

/** @brief Check whether the service is ready (engine + adapter created). */
int think_service_ready(const think_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_THINK_SERVICE_H */
