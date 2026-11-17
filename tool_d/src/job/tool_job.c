/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_job.c
 * @brief C-11 作业句柄核心：内存状态机、后台执行线程与 wire JSON。
 */

#include "tool_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airy_memory.h"
#include "error.h"
#include "platform_paths.h"
#include "platform_process.h"
#include "platform_sync.h"
#include "platform_time.h"
#include "svc_logger.h"
#include "tool_service.h"

#define TOOL_JOB_MAX_TIMEOUT 600
#define TOOL_JOB_ROOT_MAX 512

struct job_entry {
    tooljob_t *jobs;
    tool_job_info_t info;
    char *params;
    int cancel;      /* 协作取消：结果作废 */
    int thread_done;
    airy_thread_t thread;
};

struct tooljob {
    airy_mtx_t lock;
    char root[TOOL_JOB_ROOT_MAX];
    tool_job_run_t run;
    void *ctx;
    int refs;        /* 1 持有者 + 每个运行中作业 */
    int stopping;
    int count;
    unsigned seq;
    struct job_entry *slots[TOOL_JOB_SLOTS];
};

static char *job_dup(const char *s)
{
    return s ? AIRY_STRDUP(s) : NULL;
}

static void job_release(struct job_entry *e)
{
    if (!e)
        return;
    AIRY_FREE(e->info.output);
    AIRY_FREE(e->info.error);
    AIRY_FREE(e->params);
    AIRY_FREE(e);
}

static void job_put(tooljob_t *j)
{
    int dead;

    airy_mtx_lock(&j->lock);
    dead = (--j->refs <= 0);
    airy_mtx_unlock(&j->lock);
    if (!dead)
        return;
    for (int i = 0; i < TOOL_JOB_SLOTS; i++)
        job_release(j->slots[i]);
    airy_mtx_destroy(&j->lock);
    AIRY_FREE(j);
}

static struct job_entry *job_get(tooljob_t *j, const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
        if (j->slots[i] && strcmp(j->slots[i]->info.id, id) == 0)
            return j->slots[i];
    }
    return NULL;
}

static struct job_entry *job_slot(tooljob_t *j)
{
    for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
        if (!j->slots[i])
            return j->slots[i] = AIRY_CALLOC(1, sizeof(struct job_entry));
    }
    return NULL;
}

/* 容量满时淘汰最早结束的作业；全在运行中则不腾位。 */
static int job_evict(tooljob_t *j)
{
    struct job_entry *old = NULL;

    for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
        struct job_entry *e = j->slots[i];
        if (!e || e->info.state == TOOL_JOB_RUNNING)
            continue;
        if (!old || e->info.finished_ms < old->info.finished_ms)
            old = e;
    }
    if (!old)
        return AIRY_ERR_BUSY;
    for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
        if (j->slots[i] == old) {
            j->slots[i] = NULL;
            break;
        }
    }
    j->count--;
    job_release(old);
    return 0;
}

static void job_id_make(tooljob_t *j, char *buf, size_t cap)
{
    j->seq++;
    snprintf(buf, cap, "job-%llu-%04u", (unsigned long long)airy_time_wall_ms(),
             j->seq % 10000u);
}

/* 结果落账（锁内调用）：仅当未被协作取消时写入终态。 */
static void job_fill(tooljob_t *j, struct job_entry *e, int rc, tool_result_t *res)
{
    if (e->cancel) {
        e->thread_done = 1;
        return;
    }
    e->info.finished_ms = airy_time_wall_ms();
    if (res) {
        e->info.err_code = res->err_code;
        e->info.exit_code = res->exit_code;
        e->info.output = job_dup(res->output);
        e->info.error = job_dup(res->error);
        if (res->err_code == 0)
            e->info.state = TOOL_JOB_DONE;
        else if (res->err_code == AIRY_ETIMEDOUT)
            e->info.state = TOOL_JOB_TIMED_OUT;
        else
            e->info.state = TOOL_JOB_FAILED;
    } else {
        e->info.state = TOOL_JOB_FAILED;
        e->info.err_code = rc ? rc : AIRY_ERR_GENERIC_FAIL;
        e->info.error = job_dup(airy_err_str(e->info.err_code));
    }
    e->thread_done = 1;
    SVC_LOG_INFO("tool job %s finished: state=%s tool=%s", e->info.id,
                 tooljob_state(e->info.state), e->info.tool_id);
}

static void *job_worker(void *arg)
{
    struct job_entry *e = arg;
    tooljob_t *j = e->jobs;
    tool_execute_request_t req;
    tool_result_t *res = NULL;
    int rc;

    AIRY_MEMSET(&req, 0, sizeof(req));
    req.tool_id = e->info.tool_id;
    req.params_json = e->params;
    req.agent_id = e->info.agent_id[0] ? e->info.agent_id : NULL;
    req.timeout_sec = e->info.timeout_sec;
    rc = j->run(j->ctx, &req, &res);
    airy_mtx_lock(&j->lock);
    job_fill(j, e, rc, res);
    airy_mtx_unlock(&j->lock);
    tool_result_free(res);
    job_put(j);
    return NULL;
}

tooljob_t *tooljob_new(const char *root, tool_job_run_t run, void *ctx)
{
    tooljob_t *j;

    if (!run)
        return NULL;
    j = AIRY_CALLOC(1, sizeof(*j));
    if (!j)
        return NULL;
    if (airy_mtx_init(&j->lock) != 0) {
        AIRY_FREE(j);
        return NULL;
    }
    snprintf(j->root, sizeof(j->root), "%s", (root && root[0]) ? root : tooljob_root());
    j->run = run;
    j->ctx = ctx;
    j->refs = 1;
    return j;
}

void tooljob_free(tooljob_t *jobs)
{
    if (!jobs)
        return;
    airy_mtx_lock(&jobs->lock);
    jobs->stopping = 1;
    airy_mtx_unlock(&jobs->lock);
    job_put(jobs);
}

int tooljob_submit(tooljob_t *jobs, const tool_execute_request_t *req, int timeout_sec,
                   char *job_id_out, size_t cap)
{
    struct job_entry *e;

    if (!jobs || !req || !req->tool_id || !job_id_out || cap < 8)
        return AIRY_ERR_INVALID_PARAM;
    airy_mtx_lock(&jobs->lock);
    e = job_slot(jobs);
    if (!e && job_evict(jobs) == 0)
        e = job_slot(jobs);
    if (!e) {
        airy_mtx_unlock(&jobs->lock);
        return AIRY_ERR_BUSY;
    }
    e->jobs = jobs;
    job_id_make(jobs, e->info.id, sizeof(e->info.id));
    snprintf(e->info.tool_id, sizeof(e->info.tool_id), "%s", req->tool_id);
    if (req->agent_id)
        snprintf(e->info.agent_id, sizeof(e->info.agent_id), "%s", req->agent_id);
    e->info.state = TOOL_JOB_RUNNING;
    e->info.created_ms = airy_time_wall_ms();
    e->info.timeout_sec = tooljob_clamp(timeout_sec);
    e->params = job_dup(req->params_json ? req->params_json : "{}");
    jobs->count++;
    jobs->refs++;
    snprintf(job_id_out, cap, "%s", e->info.id);
    airy_mtx_unlock(&jobs->lock);

    if (airy_platform_thread_create(&e->thread, job_worker, e) != 0) {
        airy_mtx_lock(&jobs->lock);
        for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
            if (jobs->slots[i] == e) {
                jobs->slots[i] = NULL;
                break;
            }
        }
        jobs->count--;
        jobs->refs--;
        airy_mtx_unlock(&jobs->lock);
        job_release(e);
        return AIRY_ERR_SYS_THREAD;
    }
    airy_platform_thread_detach(e->thread);
    SVC_LOG_INFO("tool job %s submitted: tool=%s timeout=%ds", e->info.id,
                 e->info.tool_id, e->info.timeout_sec);
    return 0;
}

int tooljob_poll(tooljob_t *jobs, const char *job_id, tool_job_info_t *out)
{
    struct job_entry *e;

    if (!jobs || !job_id || !out)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(out, 0, sizeof(*out));
    airy_mtx_lock(&jobs->lock);
    e = job_get(jobs, job_id);
    if (!e) {
        airy_mtx_unlock(&jobs->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    *out = e->info;
    out->output = job_dup(e->info.output);
    out->error = job_dup(e->info.error);
    airy_mtx_unlock(&jobs->lock);
    return 0;
}

int tooljob_cancel(tooljob_t *jobs, const char *job_id)
{
    struct job_entry *e;

    if (!jobs || !job_id)
        return AIRY_ERR_INVALID_PARAM;
    airy_mtx_lock(&jobs->lock);
    e = job_get(jobs, job_id);
    if (!e) {
        airy_mtx_unlock(&jobs->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    if (e->info.state != TOOL_JOB_RUNNING || e->cancel) {
        airy_mtx_unlock(&jobs->lock);
        return AIRY_ERR_STATE_ERROR;
    }
    /* 协作取消：进程受 timeout 预算约束；此处立即对调用方生效，结果作废。 */
    e->cancel = 1;
    e->info.state = TOOL_JOB_CANCELLED;
    e->info.finished_ms = airy_time_wall_ms();
    airy_mtx_unlock(&jobs->lock);
    SVC_LOG_INFO("tool job %s cancelled by client", job_id);
    return 0;
}

int tooljob_count(const tooljob_t *jobs)
{
    return jobs ? jobs->count : 0;
}

const char *tooljob_state(tool_job_state_t state)
{
    switch (state) {
    case TOOL_JOB_RUNNING:
        return "running";
    case TOOL_JOB_DONE:
        return "done";
    case TOOL_JOB_FAILED:
        return "failed";
    case TOOL_JOB_CANCELLED:
        return "cancelled";
    case TOOL_JOB_TIMED_OUT:
        return "timed_out";
    default:
        return "unknown";
    }
}

cJSON *tooljob_json(const tool_job_info_t *info)
{
    cJSON *obj;

    if (!info)
        return NULL;
    obj = cJSON_CreateObject();
    if (!obj)
        return NULL;
    cJSON_AddStringToObject(obj, "job_id", info->id);
    cJSON_AddStringToObject(obj, "tool_id", info->tool_id);
    cJSON_AddStringToObject(obj, "agent_id", info->agent_id);
    cJSON_AddStringToObject(obj, "state", tooljob_state(info->state));
    cJSON_AddNumberToObject(obj, "created_ms", (double)info->created_ms);
    cJSON_AddNumberToObject(obj, "finished_ms", (double)info->finished_ms);
    cJSON_AddNumberToObject(obj, "exit_code", info->exit_code);
    cJSON_AddNumberToObject(obj, "err_code", info->err_code);
    cJSON_AddNumberToObject(obj, "timeout_sec", info->timeout_sec);
    if (info->output)
        cJSON_AddStringToObject(obj, "output", info->output);
    if (info->error)
        cJSON_AddStringToObject(obj, "error", info->error);
    return obj;
}

void tooljob_rel(tool_job_info_t *info)
{
    if (!info)
        return;
    AIRY_FREE(info->output);
    AIRY_FREE(info->error);
    info->output = NULL;
    info->error = NULL;
}

int tooljob_clamp(int timeout_sec)
{
    const char *env = getenv("AIRY_TOOL_TIMEOUT_MAX_SEC");
    int max = env ? atoi(env) : TOOL_JOB_MAX_TIMEOUT;

    if (timeout_sec <= 0)
        return 0;
    if (max < 1)
        max = TOOL_JOB_MAX_TIMEOUT;
    return timeout_sec > max ? max : timeout_sec;
}

const char *tooljob_root(void)
{
    static AIRY_THREAD_LOCAL char buf[TOOL_JOB_ROOT_MAX];
    const char *data = airy_data_dir();

    snprintf(buf, sizeof(buf), "%s/agentrt/jobs", data ? data : ".");
    return buf;
}
