// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file think_service.c
 * @brief Dual-think system service — core domain (split, 2026-08-27).
 *
 * 2026-08-27 域拆分（1067 行 → 2 文件）：
 *   - think_service.c  核心域：生命周期、GCCP 会话、认知引擎 process
 *   - think_orch.c     编排器域：orchestrator 同步执行 + ops 表注入
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
 * - Thinking events (feedback callback) are collected into a ring buffer
 *   and returned with the think.process result for TUI/upper-layer process
 *   visualization.
 */

#include "think_orch_internal.h"

/* External declaration: reactive-planner factory (coreloopthree internal
 * planner). */
extern airy_plan_strategy_t *airy_plan_reactive_create(void *llm);

static void think_sync_engine_stats(think_service_t *svc);

/* ── GCCP 会话表管理（2026-08-25 加固，调用方须已持有 svc->lock）────── */

static uint64_t think_gccp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void think_gccp_free_session(think_gccp_session_t *s)
{
    if (!s)
        return;
    AIRY_FREE(s->questions);
    s->questions = NULL;
    AIRY_FREE(s->answers);
    s->answers = NULL;
    s->session_id[0] = '\0';
    s->updated_ms = 0;
}

/* 过期会话清理（TTL 节流：每 60s 至多全表扫描一次）。 */
static void think_gccp_expire_old(think_service_t *svc)
{
    uint64_t now = think_gccp_now_ms();
    if (now - svc->gccp_last_expire_ms < 60000u)
        return;
    svc->gccp_last_expire_ms = now;
    for (uint32_t i = 0; i < THINK_GCCP_MAX_SESSIONS; i++) {
        think_gccp_session_t *s = &svc->gccp_sessions[i];
        if (s->session_id[0] && (s->questions || s->answers) &&
            now - s->updated_ms > THINK_GCCP_TTL_MS)
            think_gccp_free_session(s);
    }
}

static think_gccp_session_t *think_gccp_find(think_service_t *svc, const char *session_id)
{
    if (!svc || !session_id)
        return NULL;
    for (uint32_t i = 0; i < THINK_GCCP_MAX_SESSIONS; i++) {
        think_gccp_session_t *s = &svc->gccp_sessions[i];
        if (s->session_id[0] && strcmp(s->session_id, session_id) == 0)
            return s;
    }
    return NULL;
}

/* 查找或创建会话；槽位满时 LRU 驱逐最旧会话。 */
static think_gccp_session_t *think_gccp_get_or_create(think_service_t *svc,
                                                      const char *session_id)
{
    think_gccp_session_t *hit = think_gccp_find(svc, session_id);
    if (hit)
        return hit;

    think_gccp_session_t *free_slot = NULL;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < THINK_GCCP_MAX_SESSIONS; i++) {
        think_gccp_session_t *s = &svc->gccp_sessions[i];
        if (!s->session_id[0]) {
            free_slot = s;
            break;
        }
        if (s->updated_ms < oldest) {
            oldest = s->updated_ms;
            free_slot = s;
        }
    }
    if (!free_slot)
        return NULL;
    think_gccp_free_session(free_slot); /* 驱逐旧会话（若为 LRU 槽） */
    AIRY_STRNCPY_TERM(free_slot->session_id, session_id, sizeof(free_slot->session_id));
    free_slot->updated_ms = think_gccp_now_ms();
    return free_slot;
}

static void think_gccp_cleanup_all(think_service_t *svc)
{
    for (uint32_t i = 0; i < THINK_GCCP_MAX_SESSIONS; i++)
        think_gccp_free_session(&svc->gccp_sessions[i]);
}

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
 *   第一段（无 gccp_answers）：把 probe 的问题集序列化进当前会话
 *     （svc->active_session 对应的 gccp_sessions[] 条目），返回哨兵
 *     AIRY_GCCP_INTERACT_PENDING——引擎返回 AIRY_ERR_GCCP_INTERACTION，
 *     think_service_process 捕获后把问题集随结果 JSON 回给客户端
 *     （gccp_need_interaction=1）。
 *   第二段（携带 gccp_answers 重发）：从当前会话取出暂存的答案 JSON，
 *     引擎据此完成目标确认并正常进入后续 Phase（GCCP+GRAD 完整链路）。
 * 调用约定：svc->lock 已由 think_service_process 持有（引擎调用串行化），
 * 本回调与 process 同线程执行，直接读写 svc->gccp_sessions[] 字段，
 * 不得重复加锁。 */
static char *think_gccp_interact_cb(const airy_gccp_probe_t *probe, void *user_data)
{
    think_service_t *svc = (think_service_t *)user_data;
    if (!svc || !probe)
        return NULL;

    think_gccp_session_t *sess = think_gccp_get_or_create(svc, svc->active_session);
    if (!sess) {
        SVC_LOG_ERROR("ThinkDual: GCCP session table full, cannot record interaction");
        return AIRY_STRDUP(AIRY_GCCP_INTERACT_PENDING); /* 兜底挂起，避免误收敛 */
    }

    /* 第二段：本会话已暂存答案，直接交给引擎确认（单次有效，用完即清；
     * 同时清理问题集，避免残留到下一轮第一段）。 */
    if (sess->answers) {
        char *answers = sess->answers;
        sess->answers = NULL;
        AIRY_FREE(sess->questions);
        sess->questions = NULL;
        sess->updated_ms = think_gccp_now_ms();
        SVC_LOG_INFO("ThinkDual: GCCP answers consumed (pass 2, session=%s), resuming",
                     sess->session_id);
        return answers; /* OWNER -> airy_gccp_confirm 释放 */
    }

    /* 第一段：序列化问题集到当前会话，返回哨兵挂起本轮处理。 */
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
            AIRY_FREE(sess->questions);
            sess->questions = qjson;
            sess->updated_ms = think_gccp_now_ms();
            SVC_LOG_INFO("ThinkDual: GCCP questions captured (%zu, session=%s), pending",
                         probe->question_count, sess->session_id);
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
    think_gccp_cleanup_all(svc);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
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

int think_service_process(think_service_t *svc, const char *session_id, const char *prompt,
                          const char *gccp_answers, think_process_result_t *out_result)
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

    /* GCCP 会话隔离（2026-08-25 加固）：交互状态按 session_id 维度存取，
     * 杜绝多客户端并发串台。active_session 供回调在引擎同步调用中读取。 */
    const char *sid = (session_id && *session_id) ? session_id : "default";
    AIRY_STRNCPY_TERM(svc->active_session, sid, sizeof(svc->active_session));
    think_gccp_expire_old(svc);
    think_gccp_session_t *sess = think_gccp_get_or_create(svc, sid);

    /* GCCP 两段式交互第二段：客户端携带答案重发——暂存到当前会话，引擎
     * Phase 0 的交互回调（think_gccp_interact_cb）会取走它完成目标确认。
     * 单次有效，无论引擎是否消费，process 结束统一清理，避免泄漏到下一轮。 */
    if (gccp_answers && *gccp_answers && sess) {
        AIRY_FREE(sess->answers);
        sess->answers = AIRY_STRDUP(gccp_answers);
        sess->updated_ms = think_gccp_now_ms();
    }

    airy_task_plan_t *plan = NULL;
    airy_err_t err = airy_cognition_process(svc->engine, prompt, plen, &plan);
    if (err != AIRY_SUCCESS || !plan) {
        int err_code = (int)err;

        /* GCCP 两段式交互第一段（P-A）：引擎挂起（交互回调返回哨兵），
         * 问题集已捕获到当前会话——返回给客户端，语义为成功
         * （客户端据 gccp_need_interaction 进入问答轮）。 */
        if (err_code == AIRY_ERR_GCCP_INTERACTION) {
            SVC_LOG_INFO("ThinkDual: GCCP interaction pending (session=%s), returning questions",
                         sid);
            cJSON *root = cJSON_CreateObject();
            if (root) {
                cJSON_AddBoolToObject(root, "gccp_need_interaction", 1);
                cJSON_AddStringToObject(root, "gccp_questions",
                                        (sess && sess->questions) ? sess->questions : "[]");
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
            /* 清理本轮暂存的答案（第一段无答案；防泄漏） */
            if (sess) {
                AIRY_FREE(sess->answers);
                sess->answers = NULL;
            }
            airy_mtx_unlock(&svc->lock);
            if (!out_result->json)
                return AIRY_ERR_OUT_OF_MEMORY;
            out_result->json_len = strlen(out_result->json);
            return AIRY_SUCCESS;
        }

        /* 清理本轮暂存的交互答案（真实失败路径，防泄漏到下一轮） */
        if (sess) {
            AIRY_FREE(sess->answers);
            sess->answers = NULL;
        }
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
        if (sess) {
            AIRY_FREE(sess->answers);
            sess->answers = NULL;
        }
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
    if (sess) {
        AIRY_FREE(sess->answers);
        sess->answers = NULL;
    }
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
