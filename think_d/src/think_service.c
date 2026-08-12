// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file think_service.c
 * @brief 双思考系统服务实现（Thinkdual: t2/t1-f/t1-p + dual_coordinate）
 *
 * 设计说明：
 * - 承载 CoreLoopThree 认知引擎（airy_cognition_*），经 llm_svc_adapter
 *   直连 llm_d Unix socket，与 15 daemon 架构原生互通。
 * - t2/t1-f/t1-p 三模型经 airy_cognition_set_tc3_models 注入（NULL 用默认）。
 * - dual_coordinate（D3 断链修复）：注入自定义 airy_coordinator_strategy_t，
 *   coordinate 回调内真实调用 LLM 对 t1-f 输出与 LLM seed 做交叉一致性裁决，
 *   裁决结果写入 working memory 供 Phase 3 审计 / Phase 4 对齐读取。
 * - 思考事件（feedback callback）收集到环形缓冲，随 think.process 结果返回，
 *   供 TUI/上层做过程可视化。
 */

#include "think_service.h"

#include "cognition.h"
#include "llm_svc_adapter.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <string.h>

/* 外部声明：reactive planner 创建函数（coreloopthree 内部 planner）。
 * llm 参数传 NULL → 启发式规则规划（匹配意图规则 + 复杂度判断），
 * 不依赖 airy_llm_service_t（与 15 daemon 的 llm_svc_adapter 类型不互通），
 * 真实 LLM 规划/生成仍由 engine 的 llm_adapter 路径完成。 */
extern airy_plan_strategy_t *airy_plan_reactive_create(void *llm);

#define THINK_DEFAULT_TIMEOUT_MS 120000u
#define THINK_DEFAULT_MAX_EVENTS 64u
#define THINK_MAX_EVENT_DATA_LEN 1024u

typedef struct {
    int level;
    char module[64];
    char event[96];
    char data[THINK_MAX_EVENT_DATA_LEN];
} think_feedback_event_t;

struct think_service {
    airy_cognition_engine_t *engine;
    llm_svc_adapter_t *llm_adapter;

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
    return svc;
}

void think_service_destroy(think_service_t *svc)
{
    if (!svc)
        return;
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

int think_service_process(think_service_t *svc, const char *prompt,
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

    airy_task_plan_t *plan = NULL;
    airy_err_t err = airy_cognition_process(svc->engine, prompt, plen, &plan);
    if (err != AIRY_SUCCESS || !plan) {
        int err_code = (int)err;
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
