// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file think_orch.c
 * @brief Orchestrator integration domain (split from think_service.c, 2026-08-27).
 *
 * 2026-08-27 域拆分（think_service.c 1067 行 → 2 文件）：
 *   - think_service.c  核心服务：生命周期、GCCP 会话、认知引擎 process
 *   - think_orch.c     编排器域：orchestrator 同步执行 + ops 表注入
 *
 * S-5 (2026-08-21): orchestrator 集成
 *   - think_service_orchestrate(): 同步编排执行（RPC 后端）
 *   - think_orch_ops_*: airy_orch_ops_t 实现并注入（atoms 侧可经
 *     are_ops_get_orch() 调度编排，无需链接 daemons）
 */

#include "think_orch_internal.h"

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
