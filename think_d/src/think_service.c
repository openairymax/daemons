// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file think_service.c
 * @brief Dual-think system service implementation (Thinkdual: t2/t1-f/t1-p).
 *
 * Design notes:
 * - Hosts the CoreLoopThree cognitive engine (airy_cognition_*), connecting
 *   directly to the llm_d Unix socket via llm_svc_adapter, interoperating
 *   natively with the daemon architecture.
 * - The t2/t1-f/t1-p models are injected via airy_cognition_set_tc3_models
 *   (NULL = defaults).
 * - Dual-thinking model (2026-08-07): GCCP fact lock + GRAD plan-level
 *   logic lock. GRAD loop: t2 (A) drafts the plan -> t1-p (C) runs the
 *   deterministic four-check verifier (zero token) -> t1-f (B) makes the
 *   final accept/reject call -> converge or fall back to the seed plan.
 *   The old text-level TC3 critique loop and the dual_coordinate
 *   cross-validation were removed.
 * - Thinking events (feedback callback) are collected into a ring buffer
 *   and returned with the think.process result for TUI/upper-layer process
 *   visualization.
 */

#include "think_service.h"

#include "cognition.h"
#include "llm_svc_adapter.h"
#include "orchestrator.h"
#include "airy_orch_ops.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <pthread.h>
#include <string.h>

/* External declaration: reactive-planner factory (coreloopthree internal
 * planner). NULL llm -> heuristic rule planning (intent-rule matching +
 * complexity decision), independent of airy_llm_service_t (not
 * interoperable with the 15-daemon llm_svc_adapter type); real LLM
 * planning/generation still happens via the engine's llm_adapter path. */
extern airy_plan_strategy_t *airy_plan_reactive_create(void *llm);

#define THINK_DEFAULT_TIMEOUT_MS 120000u
#define THINK_DEFAULT_MAX_EVENTS 64u
#define THINK_MAX_EVENT_DATA_LEN 1024u
#define THINK_ORCH_MAX_RUNS 32u
#define THINK_ORCH_RUN_ID_LEN 48u

typedef struct {
    int level;
    char module[64];
    char event[96];
    char data[THINK_MAX_EVENT_DATA_LEN];
} think_feedback_event_t;

/* Orchestrator 异步运行槽（ops 表 schedule_task 的后端）：每个 run 一个
 * 线程执行 orchestrator_execute（同步管线），完成后回填结果。 */
typedef struct {
    int run_id;                 /* >0 有效；0 空槽 */
    volatile int state;         /* 1=running 2=completed 3=failed 4=cancelled */
    char *input;
    orch_result_t *results;
    size_t result_count;
    int exit_code;
    pthread_t thread;
} think_orch_run_t;

struct think_service {
    airy_cognition_engine_t *engine;
    llm_svc_adapter_t *llm_adapter;

    /* S-5 (2026-08-21): 流程编排器（与 engine_process 双管线并存） */
    orchestrator_t *orch;
    think_orch_run_t runs[THINK_ORCH_MAX_RUNS];
    int next_run_id;
    airy_mtx_t orch_lock;

    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
    uint32_t process_timeout_ms;

    think_feedback_event_t *events;
    uint32_t event_capacity;
    uint32_t event_count;
    uint32_t event_head;

    uint32_t dual_invocations;
    uint32_t dual_corrections;
    airy_mtx_t lock;

    /* P-A (2026-08-23): GCCP 两段式交互状态。交互回调与 think_service_process
     * 在同一线程同步执行（process 持 svc->lock 串行化引擎调用），故回调内
     * 直接读写本字段无需重复加锁（svc->lock 非递归，重入即死锁）。 */
    char *gccp_pending_questions; /* 第一段：probe 问题集 JSON（OWNER） */
    char *gccp_pending_answers;   /* 第二段：客户端携带的用户答案 JSON（OWNER，单次有效） */
};

static void think_sync_engine_stats(think_service_t *svc);

static void think_feedback_cb(int level, const char *module, const char *event, const char *data,
                              size_t data_len, void *user_data)
{
    think_service_t *svc = (think_service_t *)user_data;
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    think_feedback_event_t *slot = &svc->events[svc->event_head];
    slot->level = level;
    if (module)
        __builtin_memcpy(slot->module, module,
                         (strlen(module) < sizeof(slot->module)) ? strlen(module) + 1 :
                                                                   sizeof(slot->module));
    else
        slot->module[0] = '\0';
    if (event)
        __builtin_memcpy(slot->event, event,
                         (strlen(event) < sizeof(slot->event)) ? strlen(event) + 1 :
                                                                 sizeof(slot->event));
    else
        slot->event[0] = '\0';
    if (data && data_len > 0) {
        size_t n = (data_len < sizeof(slot->data) - 1) ? data_len : sizeof(slot->data) - 1;
        __builtin_memcpy(slot->data, data, n);
        slot->data[n] = '\0';
    } else {
        slot->data[0] = '\0';
    }
    svc->event_head = (svc->event_head + 1) % svc->event_capacity;
    if (svc->event_count < svc->event_capacity)
        svc->event_count++;
    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("ThinkDual feedback: level=%d module=%s event=%s", level, module ? module : "?",
                  event ? event : "?");
}

/* ── GCCP 交互回调（两段式协议, P-A 2026-08-23）─────────────────────
 *
 * think_d↔客户端是异步 RPC 往返，回调内无法同步收集答案，因此：
 *   第一段（无 gccp_answers）：把 probe 的问题集序列化进服务状态
 *     （svc->gccp_pending_questions），返回哨兵 AIRY_GCCP_INTERACT_PENDING
 *     ——引擎返回 AIRY_ERR_GCCP_INTERACTION，think_service_process 捕获后
 *     把问题集随结果 JSON 回给客户端（gccp_need_interaction=1）。
 *   第二段（携带 gccp_answers 重发）：直接返回上一轮暂存的答案 JSON，
 *     引擎据此完成目标确认并正常进入后续 Phase（GCCP+GRAD 完整链路）。
 * 调用约定：svc->lock 已由 think_service_process 持有（引擎调用串行化），
 * 本回调与 process 同线程执行，直接读写 svc->gccp_* 字段，不得重复加锁。 */
static char *think_gccp_interact_cb(const airy_gccp_probe_t *probe, void *user_data)
{
    think_service_t *svc = (think_service_t *)user_data;
    if (!svc || !probe)
        return NULL;

    /* 第二段：本请求携带了答案，直接交给引擎确认（单次有效，用完即清）。 */
    if (svc->gccp_pending_answers) {
        char *answers = svc->gccp_pending_answers;
        svc->gccp_pending_answers = NULL;
        SVC_LOG_INFO("ThinkDual: GCCP answers consumed (pass 2), resuming confirmation");
        return answers; /* OWNER -> airy_gccp_confirm 释放 */
    }

    /* 第一段：序列化问题集到服务状态，返回哨兵挂起本轮处理。 */
    cJSON *qarr = cJSON_CreateArray();
    if (qarr) {
        for (size_t i = 0; i < probe->question_count; i++) {
            const airy_gccp_question_t *q = &probe->questions[i];
            cJSON *qj = cJSON_CreateObject();
            cJSON_AddStringToObject(qj, "id", q->id);
            cJSON_AddStringToObject(qj, "question", q->question);
            cJSON_AddStringToObject(qj, "hint", q->hint);
            cJSON_AddBoolToObject(qj, "required", q->required ? 1 : 0);
            cJSON_AddItemToArray(qarr, qj);
        }
        char *qjson = cJSON_PrintUnformatted(qarr);
        cJSON_Delete(qarr);
        if (qjson) {
            AIRY_FREE(svc->gccp_pending_questions);
            svc->gccp_pending_questions = qjson;
            SVC_LOG_INFO("ThinkDual: GCCP questions captured (%zu), pending interaction",
                         probe->question_count);
        }
    }
    return AIRY_STRDUP(AIRY_GCCP_INTERACT_PENDING);
}

think_service_t *think_service_create(const think_service_config_t *config)
{
    think_service_t *svc = (think_service_t *)AIRY_CALLOC(1, sizeof(think_service_t));
    if (!svc)
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "think service alloc failed");

    airy_mtx_init(&svc->lock);

    const char *s2 = config ? config->think2_slow_model : NULL;
    const char *verify = config ? config->think1_fast_model : NULL;
    const char *expert = config ? config->think1_prof_model : NULL;
    if (s2)
        __builtin_memcpy(svc->think2_slow_model, s2,
                         (strlen(s2) < sizeof(svc->think2_slow_model)) ?
                             strlen(s2) + 1 :
                             sizeof(svc->think2_slow_model));
    if (verify)
        __builtin_memcpy(svc->think1_fast_model, verify,
                         (strlen(verify) < sizeof(svc->think1_fast_model)) ?
                             strlen(verify) + 1 :
                             sizeof(svc->think1_fast_model));
    if (expert)
        __builtin_memcpy(svc->think1_prof_model, expert,
                         (strlen(expert) < sizeof(svc->think1_prof_model)) ?
                             strlen(expert) + 1 :
                             sizeof(svc->think1_prof_model));
    svc->process_timeout_ms = (config && config->process_timeout_ms > 0) ?
                                  config->process_timeout_ms :
                                  THINK_DEFAULT_TIMEOUT_MS;
    uint32_t cap = (config && config->max_feedback_events > 0) ? config->max_feedback_events :
                                                                 THINK_DEFAULT_MAX_EVENTS;
    svc->event_capacity = cap;
    svc->events = (think_feedback_event_t *)AIRY_CALLOC(cap, sizeof(think_feedback_event_t));
    if (!svc->events) {
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "think event buffer alloc failed");
    }

    llm_svc_adapter_config_t acfg;
    __builtin_memset(&acfg, 0, sizeof(acfg));
    acfg.request_timeout_ms = svc->process_timeout_ms;
    svc->llm_adapter = llm_svc_adapter_create(&acfg);
    if (!svc->llm_adapter) {
        SVC_LOG_ERROR("ThinkDual: llm_svc_adapter create failed");
        AIRY_FREE(svc->events);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "llm adapter create failed");
    }
    SVC_LOG_INFO("ThinkDual: llm_svc_adapter created (socket=%s)",
                 llm_svc_adapter_is_connected(svc->llm_adapter) ? "connected" : "pending");

    /* 2. Cognitive engine (with a feedback callback collecting thinking
     * events). plan_strategy injects the reactive planner (llm=NULL uses
     * heuristics); otherwise airy_cognition_process's Phase 1 planning returns
     * EUNKNOWN (plan_strat stays empty). */
    airy_plan_strategy_t *plan_strat = airy_plan_reactive_create(NULL);
    if (!plan_strat) {
        SVC_LOG_ERROR("ThinkDual: reactive planner create failed");
        llm_svc_adapter_destroy(svc->llm_adapter);
        AIRY_FREE(svc->events);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "planner create failed");
    }
    SVC_LOG_INFO("ThinkDual: reactive planner created (plan=%p destroy=%p)", (void *)plan_strat,
                 plan_strat ? (void *)plan_strat->destroy : NULL);

    airy_cognition_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.cognition_default_timeout_ms = svc->process_timeout_ms;
    cfg.cognition_max_retries = 3;
    cfg.feedback_callback = think_feedback_cb;
    cfg.feedback_user_data = svc;

    airy_err_t ce = airy_cognition_create_ex_take(&cfg, plan_strat, NULL, NULL, &svc->engine);
    if (ce != AIRY_SUCCESS || !svc->engine) {
        SVC_LOG_ERROR("ThinkDual: cognition engine create failed (err=%d)", (int)ce);
        llm_svc_adapter_destroy(svc->llm_adapter);
        AIRY_FREE(svc->events);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "cognition create failed");
    }

    airy_cognition_set_llm_adapter(svc->engine, svc->llm_adapter);

    /* 4. Inject the t2/t1-f/t1-p three independent model slots (NULL ->
     * provider default). GRAD's three-model roles reuse these slots: model
     * A=t2 slow thinking (generation), model C=t1-p professional thinking
     * (quadruple-check), model B=t1-f fast thinking (final ruling). */
    airy_cognition_set_tc3_models(svc->engine,
                                  svc->think2_slow_model[0] ? svc->think2_slow_model : NULL,
                                  svc->think1_fast_model[0] ? svc->think1_fast_model : NULL,
                                  svc->think1_prof_model[0] ? svc->think1_prof_model : NULL);

    /* P-A (2026-08-23): 注册 GCCP 交互回调——两段式交互协议（无答案首轮
     * 返回问题集挂起，携带答案次轮回调返回答案继续）。此前缺失注册导致
     * 主链路走无交互降级（实测 feedback intent_confirmed 恒 interacted:0）。 */
    airy_cognition_set_gccp_interact(svc->engine, think_gccp_interact_cb, svc);
    SVC_LOG_INFO("ThinkDual: GCCP interact callback registered (two-pass protocol)");

    /* 5. Dual-thinking switch: enabled=0 -> disable the GRAD plan-level
     * critique loop (degrades to single-round planning); the t2/t1-f/t1-p
     * three roles then do not participate in critique and the plain
     * GCCP -> plan path is used. */
    if (config && config->enabled == 0) {
        airy_cognition_set_grad_enabled(svc->engine, 0);
        SVC_LOG_WARN("ThinkDual: dual-thinking disabled (enabled=0), GRAD off");
    }

    SVC_LOG_INFO("ThinkDual: service ready (engine=%p, adapter=%p, tc3=%s/%s/%s, enabled=%d)",
                 (void *)svc->engine, (void *)svc->llm_adapter,
                 svc->think2_slow_model[0] ? svc->think2_slow_model : "(default)",
                 svc->think1_fast_model[0] ? svc->think1_fast_model : "(default)",
                 svc->think1_prof_model[0] ? svc->think1_prof_model : "(default)",
                 config ? config->enabled : 1);

    /* S-5 (2026-08-21): 恢复流程编排器（orchestrator，与 engine_process
     * 双管线并存）。orchestrator 自带熔断/重试/超时/进度/取消与自定义
     * pipeline，其 LLM 调用经 ipc_service_bus 直连 llm_d。 */
    airy_mtx_init(&svc->orch_lock);
    svc->next_run_id = 1;
    orch_config_t orch_cfg;
    orch_config_get_defaults(&orch_cfg);
    orch_cfg.timeout_ms = svc->process_timeout_ms;
    svc->orch = orchestrator_create(&orch_cfg);
    if (!svc->orch) {
        SVC_LOG_ERROR("ThinkDual: orchestrator create failed");
        airy_mtx_destroy(&svc->orch_lock);
        airy_cognition_destroy(svc->engine);
        svc->engine = NULL;
        llm_svc_adapter_destroy(svc->llm_adapter);
        svc->llm_adapter = NULL;
        AIRY_FREE(svc->events);
        svc->events = NULL;
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "orchestrator create failed");
    }
    SVC_LOG_INFO("ThinkDual: orchestrator ready (timeout=%ums, retries=%u, breaker=%s)",
                 orch_cfg.timeout_ms, orch_cfg.max_retries,
                 orch_cfg.enable_circuit_breaker ? "on" : "off");
    think_orch_ops_inject(svc);
    return svc;
}

void think_service_destroy(think_service_t *svc)
{
    if (!svc)
        return;
    /* 编排器运行槽清理：释放未完成 run 的输入与结果 */
    for (uint32_t i = 0; i < THINK_ORCH_MAX_RUNS; i++) {
        think_orch_run_t *r = &svc->runs[i];
        if (r->run_id > 0) {
            AIRY_FREE(r->input);
            r->input = NULL;
            if (r->results) {
                for (size_t j = 0; j < r->result_count; j++)
                    orchestrator_result_free(&r->results[j]);
                AIRY_FREE(r->results);
                r->results = NULL;
            }
            r->run_id = 0;
            r->state = 4;
        }
    }
    if (svc->orch) {
        orchestrator_destroy(svc->orch);
        svc->orch = NULL;
    }
    airy_mtx_destroy(&svc->orch_lock);
    if (svc->engine) {
        airy_cognition_destroy(svc->engine);
        svc->engine = NULL;
    }
    if (svc->llm_adapter) {
        llm_svc_adapter_destroy(svc->llm_adapter);
        svc->llm_adapter = NULL;
    }
    AIRY_FREE(svc->events);
    svc->events = NULL;
    AIRY_FREE(svc->gccp_pending_questions);
    svc->gccp_pending_questions = NULL;
    AIRY_FREE(svc->gccp_pending_answers);
    svc->gccp_pending_answers = NULL;
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

/* ============================================================
 * S-5 (2026-08-21): orchestrator 集成
 *   - think_service_orchestrate(): 同步编排执行（RPC 后端）
 *   - think_orch_ops_*: airy_orch_ops_t 实现并注入（atoms 侧可经
 *     are_ops_get_orch() 调度编排，无需链接 daemons）
 * ============================================================ */

static think_service_t *g_think_svc = NULL;

static const char *think_orch_phase_name(orch_phase_t phase)
{
    static const char *names[] = {"decomposition", "planning", "generation", "critique",
                                  "verification",  "audit",    "alignment"};
    if (phase >= 0 && phase < ORCH_PHASE_MAX)
        return names[phase];
    return "unknown";
}

static const char *think_orch_status_name(orch_task_status_t s)
{
    static const char *names[] = {"pending", "running", "completed",
                                  "failed",  "cancelled", "timeout"};
    if (s >= 0 && s <= ORCH_TASK_TIMEOUT)
        return names[s];
    return "unknown";
}

int think_service_orchestrate(think_service_t *svc, const char *input, uint32_t timeout_ms,
                              char **out_json)
{
    if (!svc || !svc->orch || !input || !out_json) {
        SVC_LOG_ERROR("think_service_orchestrate: invalid params (svc=%p orch=%p input=%p)",
                      (void *)svc, svc ? (void *)svc->orch : NULL, (void *)input);
        return AIRY_ERR_INVALID_PARAM;
    }

    orch_result_t *results = NULL;
    size_t count = 0;
    int ret = orchestrator_execute(svc->orch, input, &results, &count);
    if (ret != 0 || !results) {
        if (results) {
            for (size_t i = 0; i < count; i++)
                orchestrator_result_free(&results[i]);
            AIRY_FREE(results);
        }
        SVC_LOG_ERROR("think_service_orchestrate: orchestrator execute failed (ret=%d)", ret);
        return ret;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        for (size_t i = 0; i < count; i++)
            orchestrator_result_free(&results[i]);
        AIRY_FREE(results);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "run_id",
                            count > 0 && results[0].task_id ? results[0].task_id : "");
    cJSON *phases = cJSON_CreateArray();
    int success = 1;
    for (size_t i = 0; i < count; i++) {
        cJSON *ph = cJSON_CreateObject();
        cJSON_AddStringToObject(ph, "phase", think_orch_phase_name((orch_phase_t)i));
        cJSON_AddStringToObject(ph, "status", think_orch_status_name(results[i].status));
        cJSON_AddNumberToObject(ph, "error_code", results[i].error_code);
        cJSON_AddNumberToObject(ph, "duration_ms", (double)results[i].duration_ms);
        cJSON_AddStringToObject(ph, "output",
                                results[i].output ? results[i].output : "");
        if (results[i].status != ORCH_TASK_COMPLETED)
            success = 0;
        cJSON_AddItemToArray(phases, ph);
    }
    cJSON_AddItemToObject(root, "phases", phases);
    cJSON_AddBoolToObject(root, "success", success ? 1 : 0);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    for (size_t i = 0; i < count; i++)
        orchestrator_result_free(&results[i]);
    AIRY_FREE(results);
    if (!json)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = json;
    return 0;
}

/* ── airy_orch_ops_t 实现（think_d 提供，atoms 经 ops 表调度编排）── */

static int think_orch_find_run(think_service_t *svc, int run_id)
{
    for (uint32_t i = 0; i < THINK_ORCH_MAX_RUNS; i++) {
        if (svc->runs[i].run_id == run_id)
            return (int)i;
    }
    return -1;
}

static void *think_orch_run_thread(void *arg)
{
    think_orch_run_t *r = (think_orch_run_t *)arg;
    think_service_t *svc = g_think_svc;
    if (!svc || !svc->orch || !r->input) {
        if (r)
            r->state = 3;
        return NULL;
    }
    r->exit_code = orchestrator_execute(svc->orch, r->input, &r->results, &r->result_count);
    if (r->state == 4) {
        /* 已取消：丢弃结果 */
        if (r->results) {
            for (size_t j = 0; j < r->result_count; j++)
                orchestrator_result_free(&r->results[j]);
            AIRY_FREE(r->results);
            r->results = NULL;
            r->result_count = 0;
        }
    } else {
        r->state = (r->exit_code == 0) ? 2 : 3;
    }
    return NULL;
}

static int think_orch_ops_schedule(const char *task_def, int priority)
{
    think_service_t *svc = g_think_svc;
    if (!svc || !task_def) {
        SVC_LOG_ERROR("orch.schedule: NULL svc=%p or task_def=%p", (void *)svc,
                      (void *)task_def);
        return -1;
    }
    (void)priority;

    airy_mtx_lock(&svc->orch_lock);
    int slot = -1;
    for (uint32_t i = 0; i < THINK_ORCH_MAX_RUNS; i++) {
        if (svc->runs[i].run_id == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        airy_mtx_unlock(&svc->orch_lock);
        SVC_LOG_WARN("orch.schedule: run table full (%u)", THINK_ORCH_MAX_RUNS);
        return -1;
    }
    think_orch_run_t *r = &svc->runs[slot];
    __builtin_memset(r, 0, sizeof(*r));
    r->run_id = svc->next_run_id++;
    if (svc->next_run_id <= 0)
        svc->next_run_id = 1;
    r->state = 1; /* running */
    r->input = AIRY_STRDUP(task_def);
    airy_mtx_unlock(&svc->orch_lock);

    if (!r->input) {
        r->run_id = 0;
        SVC_LOG_ERROR("orch.schedule: input strdup failed");
        return -1;
    }
    if (pthread_create(&r->thread, NULL, think_orch_run_thread, r) != 0) {
        airy_mtx_lock(&svc->orch_lock);
        r->run_id = 0;
        AIRY_FREE(r->input);
        r->input = NULL;
        airy_mtx_unlock(&svc->orch_lock);
        SVC_LOG_ERROR("orch.schedule: thread create failed");
        return -1;
    }
    SVC_LOG_INFO("orch.schedule: run_id=%d submitted", r->run_id);
    return r->run_id;
}

static int think_orch_ops_cancel(int task_id)
{
    think_service_t *svc = g_think_svc;
    if (!svc) {
        SVC_LOG_ERROR("orch.cancel: NULL svc");
        return AIRY_ERR_INVALID_PARAM;
    }
    airy_mtx_lock(&svc->orch_lock);
    int idx = think_orch_find_run(svc, task_id);
    if (idx < 0) {
        airy_mtx_unlock(&svc->orch_lock);
        return AIRY_ERR_NOT_FOUND;
    }
    think_orch_run_t *r = &svc->runs[idx];
    if (r->state == 1)
        r->state = 4; /* cancelled */
    if (svc->orch)
        orchestrator_cancel_all(svc->orch);
    airy_mtx_unlock(&svc->orch_lock);
    SVC_LOG_INFO("orch.cancel: run_id=%d cancelled", task_id);
    return 0;
}

static int think_orch_ops_list_pending(int **task_ids, size_t *count)
{
    think_service_t *svc = g_think_svc;
    if (!svc || !task_ids || !count) {
        if (task_ids)
            *task_ids = NULL;
        if (count)
            *count = 0;
        return AIRY_ERR_INVALID_PARAM;
    }
    airy_mtx_lock(&svc->orch_lock);
    int *ids = (int *)AIRY_CALLOC(THINK_ORCH_MAX_RUNS, sizeof(int));
    if (!ids) {
        airy_mtx_unlock(&svc->orch_lock);
        *task_ids = NULL;
        *count = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;
    for (uint32_t i = 0; i < THINK_ORCH_MAX_RUNS; i++) {
        if (svc->runs[i].run_id > 0 && svc->runs[i].state == 1)
            ids[n++] = svc->runs[i].run_id;
    }
    airy_mtx_unlock(&svc->orch_lock);
    *task_ids = ids;
    *count = n;
    return 0;
}

static void think_orch_ops_free_list(int *task_ids)
{
    AIRY_FREE(task_ids);
}

static int think_orch_ops_get_status(int task_id, int *status)
{
    think_service_t *svc = g_think_svc;
    if (!svc || !status) {
        if (status)
            *status = 0;
        return AIRY_ERR_INVALID_PARAM;
    }
    airy_mtx_lock(&svc->orch_lock);
    int idx = think_orch_find_run(svc, task_id);
    if (idx < 0) {
        airy_mtx_unlock(&svc->orch_lock);
        *status = 0;
        return AIRY_ERR_NOT_FOUND;
    }
    *status = svc->runs[idx].state;
    airy_mtx_unlock(&svc->orch_lock);
    return 0;
}

static int think_orch_ops_wait(int task_id, int timeout_ms, int *result)
{
    think_service_t *svc = g_think_svc;
    if (!svc || !result) {
        if (result)
            *result = -1;
        return AIRY_ERR_INVALID_PARAM;
    }
    airy_mtx_lock(&svc->orch_lock);
    int idx = think_orch_find_run(svc, task_id);
    airy_mtx_unlock(&svc->orch_lock);
    if (idx < 0) {
        *result = -1;
        return AIRY_ERR_NOT_FOUND;
    }

    int waited = 0;
    while (svc->runs[idx].state == 1 && (timeout_ms <= 0 || waited < timeout_ms)) {
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
        waited += 10;
    }
    if (svc->runs[idx].state == 1) {
        *result = -1;
        return AIRY_ERR_TIMEOUT;
    }
    *result = svc->runs[idx].exit_code;
    return svc->runs[idx].state == 2 ? 0 : svc->runs[idx].exit_code;
}

static const airy_orch_ops_t g_think_orch_ops = {
    .schedule_task = think_orch_ops_schedule,
    .cancel_task = think_orch_ops_cancel,
    .list_pending = think_orch_ops_list_pending,
    .free_list = think_orch_ops_free_list,
    .get_status = think_orch_ops_get_status,
    .wait = think_orch_ops_wait,
};

void think_orch_ops_inject(think_service_t *svc)
{
    g_think_svc = svc;
    are_ops_set_orch(&g_think_orch_ops);
    SVC_LOG_INFO("ThinkDual: orchestrator ops table injected (schedule/cancel/status/wait)");
}

static cJSON *think_plan_to_json(const airy_task_plan_t *plan)
{
    if (!plan)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddStringToObject(root, "task_plan_id", plan->task_plan_id ? plan->task_plan_id : "");
    cJSON_AddNumberToObject(root, "node_count", (double)plan->task_plan_node_count);
    cJSON *nodes = cJSON_CreateArray();
    for (size_t i = 0; i < plan->task_plan_node_count; i++) {
        const airy_task_node_t *n = plan->task_plan_nodes[i];
        if (!n)
            continue;
        cJSON *nj = cJSON_CreateObject();
        cJSON_AddStringToObject(nj, "id", n->task_node_id ? n->task_node_id : "");
        cJSON_AddStringToObject(nj, "goal", n->task_node_goal ? n->task_node_goal : "");
        cJSON_AddStringToObject(nj, "handler",
                                n->task_node_handler_name ? n->task_node_handler_name : "");
        cJSON_AddStringToObject(nj, "role", n->task_node_agent_role ? n->task_node_agent_role : "");
        cJSON *deps = cJSON_CreateArray();
        for (size_t d = 0; d < n->task_node_depends_count; d++) {
            if (n->task_node_depends_on && n->task_node_depends_on[d])
                cJSON_AddItemToArray(deps, cJSON_CreateString(n->task_node_depends_on[d]));
        }
        cJSON_AddItemToObject(nj, "depends", deps);
        /* GRAD quadruple-check metadata (E-01 data dependency / E-03
         * resource conservation / E-04 purpose drift): outputs inputs/outputs/
         * cost/invariant_guard in full, proving the DAG is not pure display. */
        cJSON *inputs = cJSON_CreateArray();
        for (size_t d = 0; d < n->task_node_inputs_count; d++) {
            if (n->task_node_inputs && n->task_node_inputs[d])
                cJSON_AddItemToArray(inputs, cJSON_CreateString(n->task_node_inputs[d]));
        }
        cJSON_AddItemToObject(nj, "inputs", inputs);
        cJSON *outputs = cJSON_CreateArray();
        for (size_t d = 0; d < n->task_node_outputs_count; d++) {
            if (n->task_node_outputs && n->task_node_outputs[d])
                cJSON_AddItemToArray(outputs, cJSON_CreateString(n->task_node_outputs[d]));
        }
        cJSON_AddItemToObject(nj, "outputs", outputs);
        cJSON_AddNumberToObject(nj, "cost_time_ms", (double)n->task_node_cost_time_ms);
        cJSON_AddNumberToObject(nj, "cost_mem_mb", (double)n->task_node_cost_mem_mb);
        cJSON_AddBoolToObject(nj, "invariant_guard", n->task_node_invariant_guard != 0);
        cJSON_AddItemToArray(nodes, nj);
    }
    cJSON_AddItemToObject(root, "nodes", nodes);

    cJSON *entry = cJSON_CreateArray();
    for (size_t d = 0; d < plan->task_plan_entry_count; d++) {
        if (plan->task_plan_entry_points && plan->task_plan_entry_points[d])
            cJSON_AddItemToArray(entry, cJSON_CreateString(plan->task_plan_entry_points[d]));
    }
    cJSON_AddItemToObject(root, "entry_points", entry);
    return root;
}

int think_service_process(think_service_t *svc, const char *prompt, const char *gccp_answers,
                          think_process_result_t *out_result)
{
    if (!svc || !prompt || !out_result)
        return AIRY_ERR_INVALID_PARAM;
    out_result->json = NULL;
    out_result->json_len = 0;

    size_t plen = strlen(prompt);
    if (plen == 0)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    svc->event_head = 0;
    svc->event_count = 0;
    svc->dual_invocations++;

    /* GCCP 两段式交互第二段：客户端携带答案重发——暂存答案，引擎 Phase 0
     * 的交互回调（think_gccp_interact_cb）会取走它完成目标确认。单次有效，
     * 无论引擎是否消费，process 结束统一清理，避免泄漏到下一轮调用。 */
    if (gccp_answers && *gccp_answers) {
        AIRY_FREE(svc->gccp_pending_answers);
        svc->gccp_pending_answers = AIRY_STRDUP(gccp_answers);
    }

    airy_task_plan_t *plan = NULL;
    airy_err_t err = airy_cognition_process(svc->engine, prompt, plen, &plan);
    if (err != AIRY_SUCCESS || !plan) {
        int err_code = (int)err;

        /* GCCP 两段式交互第一段（P-A）：引擎挂起（交互回调返回哨兵），
         * 问题集已捕获到 svc->gccp_pending_questions——返回给客户端，
         * 语义为成功（客户端据 gccp_need_interaction 进入问答轮）。 */
        if (err_code == AIRY_ERR_GCCP_INTERACTION) {
            SVC_LOG_INFO("ThinkDual: GCCP interaction pending, returning questions to client");
            cJSON *root = cJSON_CreateObject();
            if (root) {
                cJSON_AddBoolToObject(root, "gccp_need_interaction", 1);
                cJSON_AddStringToObject(root, "gccp_questions",
                                        svc->gccp_pending_questions ?
                                            svc->gccp_pending_questions :
                                            "[]");
                cJSON *st = cJSON_CreateObject();
                cJSON_AddNumberToObject(st, "dual_invocations", svc->dual_invocations);
                cJSON_AddNumberToObject(st, "dual_corrections", svc->dual_corrections);
                cJSON_AddItemToObject(root, "stats", st);
                cJSON *events_arr = cJSON_CreateArray();
                for (uint32_t i = 0; i < svc->event_count; i++) {
                    const think_feedback_event_t *ev = &svc->events[i];
                    cJSON *ej = cJSON_CreateObject();
                    cJSON_AddNumberToObject(ej, "level", ev->level);
                    cJSON_AddStringToObject(ej, "module", ev->module);
                    cJSON_AddStringToObject(ej, "event", ev->event);
                    cJSON_AddStringToObject(ej, "data", ev->data);
                    cJSON_AddItemToArray(events_arr, ej);
                }
                cJSON_AddItemToObject(root, "feedback", events_arr);
                out_result->json = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);
            }
            AIRY_FREE(svc->gccp_pending_answers);
            svc->gccp_pending_answers = NULL;
            airy_mtx_unlock(&svc->lock);
            if (!out_result->json)
                return AIRY_ERR_OUT_OF_MEMORY;
            out_result->json_len = strlen(out_result->json);
            return AIRY_SUCCESS;
        }

        /* 清理本轮暂存的交互答案（真实失败路径，防泄漏到下一轮） */
        AIRY_FREE(svc->gccp_pending_answers);
        svc->gccp_pending_answers = NULL;
        airy_mtx_unlock(&svc->lock);
        SVC_LOG_ERROR("ThinkDual: cognition process failed (err=%d)", err_code);

        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON *st = cJSON_CreateObject();
            cJSON_AddNumberToObject(st, "err_code", err_code);
            cJSON_AddNumberToObject(st, "dual_invocations", svc->dual_invocations);
            cJSON_AddNumberToObject(st, "dual_corrections", svc->dual_corrections);
            cJSON_AddItemToObject(root, "stats", st);
            cJSON *events_arr = cJSON_CreateArray();
            for (uint32_t i = 0; i < svc->event_count; i++) {
                const think_feedback_event_t *ev = &svc->events[i];
                cJSON *ej = cJSON_CreateObject();
                cJSON_AddNumberToObject(ej, "level", ev->level);
                cJSON_AddStringToObject(ej, "module", ev->module);
                cJSON_AddStringToObject(ej, "event", ev->event);
                cJSON_AddStringToObject(ej, "data", ev->data);
                cJSON_AddItemToArray(events_arr, ej);
            }
            cJSON_AddItemToObject(root, "feedback", events_arr);
            out_result->json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
        }
        return AIRY_ERR_UNKNOWN;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        airy_task_plan_free(plan);
        /* 清理本轮暂存的交互答案（未消费场景：need_interaction=0） */
        AIRY_FREE(svc->gccp_pending_answers);
        svc->gccp_pending_answers = NULL;
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *plan_json = think_plan_to_json(plan);
    if (plan_json)
        cJSON_AddItemToObject(root, "plan", plan_json);

    cJSON *events_arr = cJSON_CreateArray();
    for (uint32_t i = 0; i < svc->event_count; i++) {
        const think_feedback_event_t *ev = &svc->events[i];
        cJSON *ej = cJSON_CreateObject();
        cJSON_AddNumberToObject(ej, "level", ev->level);
        cJSON_AddStringToObject(ej, "module", ev->module);
        cJSON_AddStringToObject(ej, "event", ev->event);
        cJSON_AddStringToObject(ej, "data", ev->data);
        cJSON_AddItemToArray(events_arr, ej);
    }
    cJSON_AddItemToObject(root, "feedback", events_arr);

    think_sync_engine_stats(svc);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "dual_invocations", svc->dual_invocations);
    cJSON_AddNumberToObject(st, "dual_corrections", svc->dual_corrections);
    cJSON_AddNumberToObject(st, "node_count", (double)plan->task_plan_node_count);
    cJSON_AddItemToObject(root, "stats", st);

    out_result->json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    airy_task_plan_free(plan);
    /* 清理本轮暂存的交互答案（第二段回调已消费则指针已置 NULL） */
    AIRY_FREE(svc->gccp_pending_answers);
    svc->gccp_pending_answers = NULL;
    airy_mtx_unlock(&svc->lock);

    if (!out_result->json)
        return AIRY_ERR_OUT_OF_MEMORY;
    out_result->json_len = strlen(out_result->json);
    SVC_LOG_INFO("ThinkDual: process ok (prompt_len=%zu, result_len=%zu)", plen,
                 out_result->json_len);
    return AIRY_SUCCESS;
}

void think_result_free(think_process_result_t *res)
{
    if (!res)
        return;
    AIRY_FREE(res->json);
    res->json = NULL;
    res->json_len = 0;
}

/* Read the real dual-thinking stats from the engine (engine-internal
 * dual_think_corrections update points: Phase 1 S1 pre-validation failure /
 * TC3 corrections / GRAD rejections) and backfill the svc stats. */
static void think_sync_engine_stats(think_service_t *svc)
{
    if (!svc || !svc->engine)
        return;
    char *stats = NULL;
    if (airy_cognition_stats(svc->engine, &stats, NULL) != AIRY_SUCCESS || !stats)
        return;
    cJSON *root = cJSON_Parse(stats);
    AIRY_FREE(stats);
    if (!root)
        return;
    cJSON *corr = cJSON_GetObjectItem(root, "dual_think_corrections");
    if (cJSON_IsNumber(corr))
        svc->dual_corrections = (uint32_t)corr->valuedouble;
    cJSON_Delete(root);
}

char *think_service_stats_json(think_service_t *svc)
{
    if (!svc || !svc->engine)
        return NULL;
    char *health = NULL;
    airy_err_t h = airy_cognition_health_check(svc->engine, &health);
    if (h != AIRY_SUCCESS || !health)
        return NULL;

    cJSON *root = cJSON_Parse(health);
    AIRY_FREE(health);
    if (!root)
        return NULL;
    airy_mtx_lock(&svc->lock);
    think_sync_engine_stats(svc);
    cJSON_AddNumberToObject(root, "dual_invocations", svc->dual_invocations);
    cJSON_AddNumberToObject(root, "dual_corrections", svc->dual_corrections);
    cJSON_AddBoolToObject(root, "llm_adapter_connected",
                          llm_svc_adapter_is_connected(svc->llm_adapter));
    airy_mtx_unlock(&svc->lock);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

int think_service_ready(const think_service_t *svc)
{
    return (svc && svc->engine && svc->llm_adapter) ? 1 : 0;
}
