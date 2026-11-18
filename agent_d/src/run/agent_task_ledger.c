// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_task_ledger.c
 * @brief 任务事件账本写入实现（0.1.21 C-18）。
 *
 * 只做「主键选择 + 事件组装 + 调用 hall_event 写盘」，不持有状态、不
 * 触达引擎。写失败按 best effort 处理：账本不可用不能反向阻断任务执行，
 * 但事件内容本身必须是可解析的合法 JSON（hall_event 负责格式与原子写）。
 */

#include "agent_task_ledger.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "hall_event.h"

#include <string.h>

/* 统一出口：content 所有权在本函数内释放。 */
static int ledger_write(const char *task_id, const char *category, cJSON *content)
{
    if (!task_id || !task_id[0] || !category || !content)
        return -1;
    char *json = cJSON_PrintUnformatted(content);
    if (!json)
        return -1;
    int rc = hall_evt_write(task_id, category, NULL, json);
    AIRY_FREE(json);
    return rc == 0 ? 0 : -1;
}

const char *ledger_key(const agent_run_task_meta_t *task, const char *session_id)
{
    if (task && task->task_id[0])
        return task->task_id;
    if (session_id && session_id[0])
        return session_id;
    return NULL;
}

/* 状态 -> 进度：只看阶段语义，不猜测实际耗时。
 * 终态（completed/failed/cancelled）一律 1.0：任务已走完生命周期，进度
 * 不该在终态回退到 0——看板按最新事件覆盖，回退会让已失败的任务显示成
 * 「未开始」。与 gateway 看板终态兜底 (terminal ? 1 : 0) 同口径。 */
static double ledger_progress_of(const char *state)
{
    if (!state)
        return 0.0;
    if (strcmp(state, AGENT_TASK_STATE_PLANNED) == 0)
        return 0.25;
    if (strcmp(state, AGENT_TASK_STATE_EXECUTING) == 0 || strcmp(state, AGENT_TASK_STATE_PAUSED) == 0)
        return 0.5;
    if (strcmp(state, AGENT_TASK_STATE_COMPLETED) == 0 ||
        strcmp(state, AGENT_TASK_STATE_FAILED) == 0 ||
        strcmp(state, AGENT_TASK_STATE_CANCELLED) == 0)
        return 1.0;
    return 0.0;
}

int ledger_plan(const char *task_id, const char *plan_id, const char *goal,
                                const char *success_criteria)
{
    cJSON *d = cJSON_CreateObject();
    if (!d)
        return -1;
    cJSON_AddStringToObject(d, "event", "blueprint");
    cJSON_AddStringToObject(d, "task_id", task_id ? task_id : "");
    if (plan_id && plan_id[0])
        cJSON_AddStringToObject(d, "plan_id", plan_id);
    cJSON_AddStringToObject(d, "goal", goal ? goal : "");
    if (success_criteria && success_criteria[0])
        cJSON_AddStringToObject(d, "success_criteria", success_criteria);
    int rc = ledger_write(task_id, "blueprint", d);
    cJSON_Delete(d);
    return rc;
}

/* 状态事件公共体：progress/result 的字段装配一致，仅消费端事件名不同。 */
static int ledger_state_ev(const char *task_id, const char *plan_id,
                           const char *state, const char *reason,
                           const char *ev)
{
    if (!state || !state[0])
        return -1;
    cJSON *d = cJSON_CreateObject();
    if (!d)
        return -1;
    cJSON_AddStringToObject(d, "event", "task_state");
    cJSON_AddStringToObject(d, "task_id", task_id ? task_id : "");
    if (plan_id && plan_id[0])
        cJSON_AddStringToObject(d, "plan_id", plan_id);
    cJSON_AddStringToObject(d, "state", state);
    cJSON_AddNumberToObject(d, "progress", ledger_progress_of(state));
    if (reason && reason[0])
        cJSON_AddStringToObject(d, "reason", reason);
    int rc = ledger_write(task_id, ev, d);
    cJSON_Delete(d);
    return rc;
}

int ledger_progress(const char *task_id, const char *plan_id, const char *state,
                               const char *reason)
{
    return ledger_state_ev(task_id, plan_id, state, reason, "progress");
}

int ledger_issue(const char *task_id, int rc, const char *reason)
{
    cJSON *d = cJSON_CreateObject();
    if (!d)
        return -1;
    cJSON_AddStringToObject(d, "event", "issue");
    cJSON_AddStringToObject(d, "task_id", task_id ? task_id : "");
    cJSON_AddNumberToObject(d, "rc", rc);
    if (reason && reason[0])
        cJSON_AddStringToObject(d, "reason", reason);
    int wr = ledger_write(task_id, "issue", d);
    cJSON_Delete(d);
    return wr;
}

int ledger_result(const char *task_id, const char *plan_id, const char *state)
{
    return ledger_state_ev(task_id, plan_id, state, NULL, "result");
}

int ledger_cancel(const char *task_id, const char *plan_id, const char *by,
                             uint64_t ts_ms)
{
    cJSON *d = cJSON_CreateObject();
    if (!d)
        return -1;
    cJSON_AddStringToObject(d, "event", "task_state");
    cJSON_AddStringToObject(d, "task_id", task_id ? task_id : "");
    if (plan_id && plan_id[0])
        cJSON_AddStringToObject(d, "plan_id", plan_id);
    cJSON_AddStringToObject(d, "state", AGENT_TASK_STATE_CANCELLED);
    cJSON_AddNumberToObject(d, "progress", ledger_progress_of(AGENT_TASK_STATE_CANCELLED));
    if (by && by[0])
        cJSON_AddStringToObject(d, AIRY_RS_K_CANCELLED_BY, by);
    if (ts_ms)
        cJSON_AddNumberToObject(d, AIRY_RS_K_CANCELLED_AT, (double)ts_ms);
    int rc = ledger_write(task_id, "result", d);
    cJSON_Delete(d);
    return rc;
}
