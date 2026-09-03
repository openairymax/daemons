// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_service_worker.c
 * @brief Scheduler service - task-queue worker thread domain.
 * @details Implements the worker thread consuming the priority ring queue
 *          (highest priority first, FIFO within the same priority) and the
 *          worker start/stop public APIs in scheduler_service.h. The
 *          queue-take helper and the worker-thread entry are file-local
 *          statics; the work-hall event helpers (sched_hall_*) are shared
 *          with sched_service_task.c via sched_service_internal.h.
 *          Moved out of sched_service_impl.c (single-responsibility split):
 *          the service create/destroy lifecycle lives in sched_service_impl.c,
 *          the task queue APIs in sched_service_task.c.
 */

#include "scheduler_service.h"
#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Pick and remove the highest-priority task from the ring queue (FIFO among
 * equal priorities, taking the earliest enqueued). Caller must hold the lock;
 * returns NULL when the queue is empty. Compact removal keeps the ring
 * semantics hole-free (consistent with cancel_task's queue compaction).
 */
static task_record_t *sched_queue_take_highest(sched_service_t *svc)
{
    if (svc->queue_head == svc->queue_tail)
        return NULL;

    size_t best = svc->queue_head;
    task_priority_t best_p = svc->queue[best]->priority;
    for (size_t qi = svc->queue_head; qi != svc->queue_tail; qi = (qi + 1) % AIRY_CAP_MAX_TASKS) {
        if (svc->queue[qi]->priority > best_p) {
            best = qi;
            best_p = svc->queue[qi]->priority;
        }
    }
    task_record_t *rec = svc->queue[best];

    size_t src = (best + 1) % AIRY_CAP_MAX_TASKS;
    size_t dst = best;
    while (src != svc->queue_tail) {
        svc->queue[dst] = svc->queue[src];
        src = (src + 1) % AIRY_CAP_MAX_TASKS;
        dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
    }
    svc->queue_tail = (svc->queue_tail + AIRY_CAP_MAX_TASKS - 1) % AIRY_CAP_MAX_TASKS;
    SVC_LOG_DEBUG("sched: queue take: task=%s priority=%d wait_ms=%llu remain=%zu", rec->task_id,
                  (int)rec->priority, (unsigned long long)(sched_now_ms() - rec->created_at_ms),
                  (size_t)((svc->queue_tail + AIRY_CAP_MAX_TASKS - svc->queue_head) % AIRY_CAP_MAX_TASKS));
    return rec;
}

/*
 * Worker thread: consumes the pending queue by priority (highest first, FIFO
 * within the same priority), running select agent -> executor (spawn+invoke) ->
 * status write-back. The lock is held only for queue operations/status
 * write-back; selection and dispatch (time-consuming) run outside the lock so
 * long tasks do not block register_agent and other RPCs.
 */
static void *sched_worker_thread(void *arg)
{
    sched_service_t *svc = (sched_service_t *)arg;

    while (1) {
        airy_mtx_lock(&svc->lock);

        while (svc->queue_head == svc->queue_tail && svc->worker_run) {
            SVC_LOG_DEBUG("sched: worker idle, waiting for tasks");
            airy_cond_wait(&svc->queue_cond, &svc->lock);
        }
        if (!svc->worker_run) {
            SVC_LOG_INFO("sched: worker received stop signal, exiting");
            airy_mtx_unlock(&svc->lock);
            break;
        }
        task_record_t *rec = sched_queue_take_highest(svc);
        if (!rec) {
            SVC_LOG_WARN("sched: worker woke but queue empty (spurious wakeup?)");
            airy_mtx_unlock(&svc->lock);
            continue;
        }
        rec->status = SCHED_TASK_STATUS_RUNNING;
        const uint64_t wait_ms = sched_now_ms() - rec->created_at_ms;
        SVC_LOG_INFO("sched: task dequeued: %s (priority=%d, wait_ms=%llu, "
                     "desc_len=%zu, timeout_ms=%u)",
                     rec->task_id, (int)rec->priority, (unsigned long long)wait_ms,
                     rec->task_description ? strlen(rec->task_description) : 0, rec->timeout_ms);
        airy_mtx_unlock(&svc->lock);

        char *selected = NULL;
        char *output = NULL;
        char *error = NULL;
        const uint64_t exec_t0 = sched_now_ms();

        sched_task_info_t tinfo;
        __builtin_memset(&tinfo, 0, sizeof(tinfo));
        tinfo.task_id = rec->task_id;
        tinfo.task_description = rec->task_description;
        tinfo.priority = rec->priority;
        tinfo.timeout_ms = rec->timeout_ms;

        sched_result_t *sel = NULL;
        int sret = sched_service_schedule_task(svc, &tinfo, &sel);
        if (sret != AIRY_SUCCESS || !sel || !sel->selected_agent_id) {
            SVC_LOG_ERROR("sched: task %s select agent failed (rc=%d, sel=%p) — "
                          "check agent_d register_agent and agent availability",
                          rec->task_id, sret, (const void *)sel);
            error = AIRY_STRDUP("no available agent registered");
        } else {
            selected = AIRY_STRDUP(sel->selected_agent_id);
            if (!svc->executor) {
                sret = AIRY_ERR_SVC_NOT_READY;
                error = AIRY_STRDUP("task executor not injected");
                SVC_LOG_ERROR("sched: task %s executor not injected (set_executor "
                              "must be called before start_workers)",
                              rec->task_id);
            } else {
                SVC_LOG_INFO("sched: dispatching task %s to agent %s", rec->task_id,
                             sel->selected_agent_id);
                /* 执行链事件（2.8b）：任务开始执行 → progress 事件。 */
                sched_hall_progress(rec->task_id, "task_started", (int)rec->priority);
                sret = svc->executor(sel->selected_agent_id, rec->task_description, NULL, &output);
                if (sret != AIRY_SUCCESS || !output) {
                    SVC_LOG_ERROR("sched: executor returned failure for task %s "
                                  "(rc=%d, output=%p)",
                                  rec->task_id, sret, (const void *)output);
                    error = AIRY_STRDUP("agent dispatch failed");
                }
            }
        }
        if (sel) {
            AIRY_FREE(sel->selected_agent_id);
            AIRY_FREE(sel);
        }

        const uint64_t exec_elapsed_ms = sched_now_ms() - exec_t0;

        airy_mtx_lock(&svc->lock);
        rec->selected_agent_id = selected;
        selected = NULL;
        rec->finished_at_ms = sched_now_ms();
        if (sret == AIRY_SUCCESS && output) {
            if (rec->timeout_ms > 0 && exec_elapsed_ms > rec->timeout_ms) {
                /* Execution returned success but exceeded the task-level
                 * timeout: report as timeout failure, closing the timeout_ms
                 * semantic loop (the synchronous executor cannot be
                 * interrupted mid-flight, at least the result layer applies). */
                rec->status = SCHED_TASK_STATUS_FAILED;
                rec->error = AIRY_STRDUP("task timed out");
                AIRY_FREE(output);
                output = NULL;
                SVC_LOG_WARN("Task timed out: %s (elapsed=%llu ms, limit=%u ms)", rec->task_id,
                             (unsigned long long)exec_elapsed_ms, rec->timeout_ms);
            } else {
                rec->status = SCHED_TASK_STATUS_COMPLETED;
                rec->output = output;
                output = NULL;
                SVC_LOG_INFO("Task completed: %s (agent=%s, output_len=%zu)", rec->task_id,
                             rec->selected_agent_id ? rec->selected_agent_id : "?",
                             rec->output ? strlen(rec->output) : 0);
            }
        } else {
            rec->status = SCHED_TASK_STATUS_FAILED;
            rec->error = error;
            error = NULL;
            SVC_LOG_ERROR("Task failed: %s (agent=%s, error=%s)", rec->task_id,
                          rec->selected_agent_id ? rec->selected_agent_id : "?",
                          rec->error ? rec->error : "unknown");
        }
        airy_mtx_unlock(&svc->lock);

        /* 执行链事件（2.8b）：任务终态 → result 事件（completed/failed/canceled）。
         * best-effort，绝不影响任务结果回写。 */
        static const char *hw_status_names[] = {"pending", "running", "completed", "failed",
                                                "canceled"};
        const char *sname2 = (rec->status < SCHED_TASK_STATUS_COUNT) ?
                                 hw_status_names[rec->status] :
                                 "unknown";
        sched_hall_result(rec->task_id, rec->selected_agent_id, sname2, exec_elapsed_ms,
                          rec->output, rec->error);

        AIRY_FREE(selected);
        AIRY_FREE(output);
        AIRY_FREE(error);
    }
    return NULL;
}

int sched_service_start_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;
    if (service->worker_thread != AIRY_INVALID_THREAD)
        return AIRY_SUCCESS;

    service->worker_run = 1;
    if (airy_thread_create(&service->worker_thread, sched_worker_thread, service) != 0) {
        service->worker_run = 0;
        service->worker_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler worker thread started");

    service->dag_run = 1;
    if (airy_thread_create(&service->dag_thread, sched_dag_worker_thread, service) != 0) {
        service->dag_run = 0;
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: dag thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler DAG worker thread started");
    return AIRY_SUCCESS;
}

void sched_service_stop_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return;

    /* Stop the DAG thread first, then the task queue thread (coordinated via
     * the same dag_cond/lock). The dag thread may block on batch_cond (parallel
     * batch barrier) or dag_cond; both are broadcast. */
    if (service->dag_thread != AIRY_INVALID_THREAD) {
        airy_mtx_lock(&service->lock);
        service->dag_run = 0;
        airy_cond_broadcast(&service->dag_cond);
        airy_cond_broadcast(&service->batch_cond);
        airy_mtx_unlock(&service->lock);
        airy_thread_join(service->dag_thread, NULL);
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_INFO("Scheduler DAG worker thread stopped");
    }

    if (service->worker_thread == AIRY_INVALID_THREAD)
        return;

    airy_mtx_lock(&service->lock);
    service->worker_run = 0;

    airy_cond_broadcast(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    airy_thread_join(service->worker_thread, NULL);
    service->worker_thread = AIRY_INVALID_THREAD;
    SVC_LOG_INFO("Scheduler worker thread stopped");
}
