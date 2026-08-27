// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_service_task.c
 * @brief Scheduler service - task lifecycle / work-hall event domain.
 * @details Implements the async task-queue public APIs in scheduler_service.h
 *          (submit/get/cancel task, set_executor) and the work-hall event
 *          wiring (best-effort progress/result writes, shared with the
 *          worker thread in sched_service_worker.c). Moved out of
 *          sched_service_impl.c (single-responsibility split): the service
 *          create/destroy lifecycle lives in sched_service_impl.c, the agent
 *          registry/strategy domain in sched_service_agent.c, and the worker
 *          thread in sched_service_worker.c.
 */

#include "scheduler_service.h"
#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "platform.h"
#include "hall_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

/* 执行链事件接线（2.8b）：将调度任务生命周期事件写入事件流单一真相源。
 * best-effort：写失败仅记录日志，绝不失败任务流程。
 * 被 sched_service_task.c（submit/cancel）与 sched_service_worker.c（worker
 * 线程）共享，故为非 static（声明见 sched_service_internal.h）。 */
void sched_hall_progress(const char *task_id, const char *event, int priority)
{
    if (!task_id || !task_id[0] || !event)
        return;
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", event);
    cJSON_AddNumberToObject(evt, "priority", (double)priority);
    char *s = cJSON_PrintUnformatted(evt);
    if (s) {
        (void)daemon_hall_write(task_id, "progress", NULL, s);
        cJSON_free(s);
    }
    cJSON_Delete(evt);
}

void sched_hall_result(const char *task_id, const char *agent, const char *status,
                       uint64_t elapsed_ms, const char *output, const char *error)
{
    if (!task_id || !task_id[0] || !status)
        return;
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "task_done");
    cJSON_AddStringToObject(evt, "status", status);
    if (agent && agent[0])
        cJSON_AddStringToObject(evt, "agent", agent);
    cJSON_AddNumberToObject(evt, "elapsed_ms", (double)elapsed_ms);
    if (output && output[0])
        cJSON_AddNumberToObject(evt, "output_len", (double)strlen(output));
    if (error && error[0])
        cJSON_AddStringToObject(evt, "error", error);
    char *s = cJSON_PrintUnformatted(evt);
    if (s) {
        (void)daemon_hall_write(task_id, "result", NULL, s);
        cJSON_free(s);
    }
    cJSON_Delete(evt);
}

int sched_service_submit_task(sched_service_t *service, const task_info_t *task_info,
                              char **out_task_id)
{
    if (!service || !task_info || !out_task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_submit_task: NULL parameter or not initialized (service=%p, "
                      "task_info=%p, out_task_id=%p, initialized=%d)",
                      (const void *)service, (const void *)task_info, (const void *)out_task_id,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_task_id = NULL;

    /* task_id: use the client-provided one, else generate server-side
     * (timestamp+sequence, guaranteed unique). Generation must happen under
     * the lock — RPCs are processed concurrently through the thread pool
     * (thread_pool_max=8); task_seq++ outside the lock would race and produce
     * duplicate task_ids (data race). */
    char id_buf[64];
    const int use_generated = !(task_info->task_id && task_info->task_id[0]);

    airy_mtx_lock(&service->lock);
    if (use_generated) {
        snprintf(id_buf, sizeof(id_buf), "task_%llu_%zu", (unsigned long long)time(NULL),
                 service->task_seq++);
    } else {
        snprintf(id_buf, sizeof(id_buf), "%s", task_info->task_id);
    }
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, id_buf) == 0) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_ERROR("sched_service_submit_task: duplicate task_id: %s", id_buf);
            return AIRY_ERR_ALREADY_EXISTS;
        }
    }
    size_t qlen =
        (service->queue_tail + AIRY_CAP_MAX_TASKS - service->queue_head) % AIRY_CAP_MAX_TASKS;
    if (qlen >= AIRY_CAP_MAX_TASKS - 1 || service->task_count >= AIRY_CAP_MAX_TASKS) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: task queue full (task_count=%zu)",
                      service->task_count);
        return AIRY_ERR_OVERFLOW;
    }

    task_record_t *rec = (task_record_t *)AIRY_CALLOC(1, sizeof(task_record_t));
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: calloc failed for task record");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->task_id = AIRY_STRDUP(id_buf);
    rec->task_description =
        AIRY_STRDUP(task_info->task_description ? task_info->task_description : "");
    if (!rec->task_id || !rec->task_description) {
        AIRY_FREE(rec->task_id);
        AIRY_FREE(rec->task_description);
        AIRY_FREE(rec);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->priority = task_info->priority;
    rec->timeout_ms = task_info->timeout_ms;
    rec->status = SCHED_TASK_STATUS_PENDING;
    rec->created_at_ms = sched_now_ms();

    service->tasks[service->task_count++] = rec;
    service->queue[service->queue_tail] = rec;
    service->queue_tail = (service->queue_tail + 1) % AIRY_CAP_MAX_TASKS;

    airy_cond_signal(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    *out_task_id = AIRY_STRDUP(id_buf);
    SVC_LOG_INFO("Task queued: %s (priority=%d, timeout_ms=%u, queue_depth=%zu, "
                 "desc_len=%zu, worker=%s)",
                 id_buf, (int)task_info->priority, task_info->timeout_ms, qlen + 1,
                 rec->task_description ? strlen(rec->task_description) : 0,
                 service->worker_run ? "running" : "NOT STARTED (task will stay pending)");

    /* 执行链事件（2.8b）：任务入队 → progress 事件。 */
    sched_hall_progress(id_buf, "task_queued", (int)task_info->priority);
    return AIRY_SUCCESS;
}

int sched_service_get_task(sched_service_t *service, const char *task_id, char **out_json)
{
    if (!service || !task_id || !out_json || !service->initialized) {
        SVC_LOG_ERROR("sched_service_get_task: NULL parameter or not initialized (service=%p, "
                      "task_id=%p, out_json=%p, initialized=%d)",
                      (const void *)service, (const void *)task_id, (const void *)out_json,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    task_record_t *rec = NULL;
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, task_id) == 0) {
            rec = service->tasks[i];
            break;
        }
    }
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_WARN("sched_service_get_task: task not found (task_id=%s)", task_id);
        return AIRY_ERR_NOT_FOUND;
    }

    static const char *status_names[] = {"pending", "running", "completed", "failed", "canceled"};
    const char *sname =
        (rec->status < SCHED_TASK_STATUS_COUNT) ? status_names[rec->status] : "unknown";

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "task_id", rec->task_id);
        cJSON_AddStringToObject(root, "status", sname);
        cJSON_AddNumberToObject(root, "priority", (double)rec->priority);
        cJSON_AddNumberToObject(root, "timeout_ms", (double)rec->timeout_ms);
        cJSON_AddStringToObject(root, "selected_agent_id",
                                rec->selected_agent_id ? rec->selected_agent_id : "");
        cJSON_AddStringToObject(root, "output", rec->output ? rec->output : "");
        cJSON_AddStringToObject(root, "error", rec->error ? rec->error : "");
        cJSON_AddNumberToObject(root, "created_at_ms", (double)rec->created_at_ms);
        cJSON_AddNumberToObject(root, "finished_at_ms", (double)rec->finished_at_ms);
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}

int sched_service_set_executor(sched_service_t *service, sched_task_executor_t executor)
{
    if (!service || !executor || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    service->executor = executor;
    return AIRY_SUCCESS;
}

int sched_service_cancel_task(sched_service_t *service, const char *task_id)
{
    if (!service || !task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_cancel_task: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    int found = 0;
    for (size_t i = 0; i < service->task_count; i++) {
        task_record_t *rec = service->tasks[i];
        if (strcmp(rec->task_id, task_id) != 0)
            continue;
        found = 1;
        if (rec->status != SCHED_TASK_STATUS_PENDING) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_WARN("sched_service_cancel_task: task %s not cancelable (status=%d)", task_id,
                         (int)rec->status);
            return AIRY_ERR_BUSY;
        }

        size_t qi = service->queue_head;
        size_t removed = 0;
        while (qi != service->queue_tail) {
            if (service->queue[qi] == rec) {
                service->queue[qi] = NULL;
                removed = 1;
                break;
            }
            qi = (qi + 1) % AIRY_CAP_MAX_TASKS;
        }
        if (removed) {

            size_t src = (qi + 1) % AIRY_CAP_MAX_TASKS;
            size_t dst = qi;
            while (src != service->queue_tail) {
                service->queue[dst] = service->queue[src];
                src = (src + 1) % AIRY_CAP_MAX_TASKS;
                dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
            }
            service->queue_tail =
                (service->queue_tail + AIRY_CAP_MAX_TASKS - 1) % AIRY_CAP_MAX_TASKS;
        }
        rec->status = SCHED_TASK_STATUS_CANCELED;
        rec->finished_at_ms = sched_now_ms();
        rec->error = AIRY_STRDUP("canceled by user");
        SVC_LOG_INFO("Task canceled: %s (removed_from_queue=%d, pending_in_queue=%zu)", task_id,
                     (int)removed,
                     (service->queue_tail + AIRY_CAP_MAX_TASKS - service->queue_head) %
                         AIRY_CAP_MAX_TASKS);
        break;
    }
    airy_mtx_unlock(&service->lock);

    /* 执行链事件（2.8b）：任务取消 → progress 事件。 */
    if (found)
        sched_hall_progress(task_id, "task_canceled", 0);

    return found ? AIRY_SUCCESS : AIRY_ERR_NOT_FOUND;
}
