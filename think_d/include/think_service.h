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
 * @param svc Service handle
 * @param prompt User input (UTF-8)
 * @param out_result Output result (OWNER, caller frees via think_result_free)
 * @return 0 on success, non-zero on failure
 */
int think_service_process(think_service_t *svc, const char *prompt,
                          think_process_result_t *out_result);

void think_result_free(think_process_result_t *res);


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
