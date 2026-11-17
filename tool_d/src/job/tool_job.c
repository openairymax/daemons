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
#include "io.h"
#include "platform_misc.h"
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

static void job_path(tooljob_t *j, const char *id, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s/%s.json", j->root, id);
}

/* 终态记录落盘：单文件覆盖写，读回失败按无记录处理。 */
static void job_save(tooljob_t *j, const tool_job_info_t *info)
{
    char path[TOOL_JOB_ROOT_MAX + TOOL_JOB_ID_MAX + 16];
    cJSON *obj = tooljob_json(info);
    char *text;

    if (!obj)
        return;
    text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text)
        return;
    job_path(j, info->id, path, sizeof(path));
    if (airy_io_write_file(path, text, (size_t)-1) != 0)
        SVC_LOG_WARN("tool job %s persist failed: %s", info->id, path);
    cJSON_free(text);
}

static void job_field(cJSON *obj, const char *key, char *buf, size_t cap)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsString(it) && it->valuestring)
        snprintf(buf, cap, "%s", it->valuestring);
}

static int job_state_parse(const char *name, tool_job_state_t *out)
{
    if (!name)
        return AIRY_ERR_INVALID_PARAM;
    for (int s = TOOL_JOB_RUNNING; s <= TOOL_JOB_TIMED_OUT; s++) {
        if (strcmp(tooljob_state((tool_job_state_t)s), name) == 0) {
            *out = (tool_job_state_t)s;
            return 0;
        }
    }
    return AIRY_ERR_INVALID_PARAM;
}

/* 解析落盘记录；失败时不占用 out 的 output/error（调用方可直接丢弃）。 */
static int job_parse(const char *text, tool_job_info_t *out)
{
    cJSON *obj = cJSON_Parse(text);
    cJSON *it;
    int rc = AIRY_ERR_INVALID_PARAM;

    if (!obj)
        return rc;
    AIRY_MEMSET(out, 0, sizeof(*out));
    job_field(obj, "job_id", out->id, sizeof(out->id));
    job_field(obj, "tool_id", out->tool_id, sizeof(out->tool_id));
    job_field(obj, "agent_id", out->agent_id, sizeof(out->agent_id));
    it = cJSON_GetObjectItemCaseSensitive(obj, "state");
    if (!out->id[0] || !cJSON_IsString(it) ||
        job_state_parse(it->valuestring, &out->state) != 0)
        goto done;
    it = cJSON_GetObjectItemCaseSensitive(obj, "created_ms");
    out->created_ms = cJSON_IsNumber(it) ? (uint64_t)it->valuedouble : 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, "finished_ms");
    out->finished_ms = cJSON_IsNumber(it) ? (uint64_t)it->valuedouble : 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, "exit_code");
    out->exit_code = cJSON_IsNumber(it) ? it->valueint : 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, "err_code");
    out->err_code = cJSON_IsNumber(it) ? it->valueint : 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, "timeout_sec");
    out->timeout_sec = cJSON_IsNumber(it) ? it->valueint : 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, "output");
    if (cJSON_IsString(it) && it->valuestring)
        out->output = job_dup(it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(obj, "error");
    if (cJSON_IsString(it) && it->valuestring)
        out->error = job_dup(it->valuestring);
    rc = 0;
done:
    cJSON_Delete(obj);
    return rc;
}

/* 容量满时淘汰最早结束的作业；全在运行中则不腾位。 */
static int job_evict(tooljob_t *j)
{
    char path[TOOL_JOB_ROOT_MAX + TOOL_JOB_ID_MAX + 16];
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
    job_path(j, old->info.id, path, sizeof(path));
    job_release(old);
    airy_io_remove_file(path);
    return 0;
}

/* 启动时读回历史记录；上次运行中的作业视为守护重启中断，改为失败。 */
static void job_load(tooljob_t *j)
{
    char path[TOOL_JOB_ROOT_MAX + TOOL_JOB_ID_MAX + 16];
    char **files = NULL;
    size_t n = 0;

    if (airy_io_mkdir_p(j->root, 0755) != 0) {
        SVC_LOG_WARN("tool job root unavailable: %s", j->root);
        return;
    }
    if (airy_io_list_files(j->root, &files, &n) != 0)
        return;
    for (size_t i = 0; i < n; i++) {
        tool_job_info_t info;
        struct job_entry *e;
        char *text = NULL;

        if (!strstr(files[i], ".json"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", j->root, files[i]);
        if (airy_io_read_file(path, &text, NULL) != 0)
            continue;
        if (job_parse(text, &info) != 0) {
            AIRY_FREE(text);
            continue;
        }
        AIRY_FREE(text);
        if (info.state == TOOL_JOB_RUNNING) {
            info.state = TOOL_JOB_FAILED;
            info.err_code = AIRY_ERR_STATE_ERROR;
            info.finished_ms = airy_time_wall_ms();
            info.error = job_dup("daemon restarted during job");
            job_save(j, &info);
        }
        e = job_slot(j);
        if (!e && job_evict(j) == 0)
            e = job_slot(j);
        if (!e) {
            tooljob_rel(&info);
            break;
        }
        e->jobs = j;
        e->info = info;
        j->count++;
    }
    airy_io_free_list(files, n);
    if (j->count)
        SVC_LOG_INFO("tool job store loaded %d record(s) from %s", j->count, j->root);
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
    job_save(j, &e->info);
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
    job_load(j);
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
    job_save(jobs, &e->info);
    airy_mtx_unlock(&jobs->lock);

    if (airy_platform_thread_create(&e->thread, job_worker, e) != 0) {
        char path[TOOL_JOB_ROOT_MAX + TOOL_JOB_ID_MAX + 16];

        airy_mtx_lock(&jobs->lock);
        job_path(jobs, e->info.id, path, sizeof(path));
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
        airy_io_remove_file(path);
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
    job_save(jobs, &e->info);
    airy_mtx_unlock(&jobs->lock);
    SVC_LOG_INFO("tool job %s cancelled by client", job_id);
    return 0;
}

int tooljob_count(const tooljob_t *jobs)
{
    return jobs ? jobs->count : 0;
}

int tooljob_drain(tooljob_t *jobs, int timeout_ms)
{
    int waited = 0;

    if (!jobs)
        return AIRY_ERR_INVALID_PARAM;
    for (;;) {
        int busy = 0;

        airy_mtx_lock(&jobs->lock);
        for (int i = 0; i < TOOL_JOB_SLOTS; i++) {
            if (jobs->slots[i] && jobs->slots[i]->info.state == TOOL_JOB_RUNNING)
                busy++;
        }
        airy_mtx_unlock(&jobs->lock);
        if (!busy)
            return 0;
        if (waited >= timeout_ms)
            return AIRY_ETIMEDOUT;
        airy_sleep_ms(20);
        waited += 20;
    }
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
