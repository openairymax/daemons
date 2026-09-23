// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file executor_pool.c
 * @brief R1-a (0.1.17): tool 执行面专用有界 worker 池实现。
 *
 * 模型：提交方深拷贝 job 入队（有界背压），worker 串行复用既有
 * tool_executor_run 全链（审批/读写门控/预算/sandbox 不重写）；提交方
 * 带预算等待，超时把 job 置 detached 后返回 AIRY_ERR_CANCELED，worker
 * 完成该 job 时自行释放（真实结果被丢弃——取消语义：上层以合成的
 * NORMAL_FAIL 取消结果回报调用方）。
 *
 * 所有权（job 指针共享，全部状态迁移在池锁内判定，无竞态）：
 *   - 正常完成：等待方取走 result 并释放 job 其余部分；
 *   - detach：worker 在完成时释放整个 job（含 result）；
 *   - 关闭残留：worker/destroy 置 done + 合成取消结果，等待方释放 job。
 */

#include "airy_memory.h"
#include "error.h"
#include "executor.h"
#include "executor_pool.h"
#include "platform_misc.h"
#include "platform_process.h"
#include "svc_logger.h"
#include "tool_interactive_approval.h"

#include <stdlib.h>
#include <string.h>

#define ENV_EXEC_WORKERS "AIRY_TOOL_EXEC_WORKERS"
#define ENV_WAIT_BUDGET "AIRY_TOOL_WAIT_BUDGET_MS"
#define POOL_QUEUE_CAP 64
#define POOL_WORKER_MAX 8
#define POOL_WORKER_DEFAULT 2
#define WAIT_SLACK_MS 2000u
#define WAIT_SLICE_MS 100u

typedef struct pool_job {
    tool_metadata_t meta; /* 深拷贝（service 层在返回后即释放原 meta） */
    char *params_json;
    char *agent_id;
    tool_result_t *result;
    int ret;
    int done;
    int detached;
} pool_job_t;

struct executor_pool {
    tool_executor_t *exec; /* BORROW */
    airy_mtx_t lock;
    airy_cond_t work_cond; /* worker 等 job */
    airy_cond_t done_cond; /* 等待方/destroy 等 job 完成 */
    pool_job_t *queue[POOL_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int running; /* 正在 worker 内执行的 job 数 */
    int shutdown;
    int worker_count;
    airy_thread_t *threads;
};

/* ---------- job 生命周期 ---------- */

static char *dup_str(const char *s)
{
    return s ? AIRY_STRDUP(s) : NULL;
}

static tool_param_t *params_deep_copy(const tool_param_t *src, size_t n)
{
    if (!src || n == 0)
        return NULL;
    tool_param_t *arr = (tool_param_t *)AIRY_CALLOC(n, sizeof(tool_param_t));
    if (!arr)
        return NULL;
    for (size_t i = 0; i < n; ++i) {
        arr[i].name = dup_str(src[i].name);
        arr[i].schema = dup_str(src[i].schema);
        arr[i].required = src[i].required;
    }
    return arr;
}

static void meta_copy_free(tool_metadata_t *m)
{
    if (!m)
        return;
    AIRY_FREE(m->id);
    AIRY_FREE(m->name);
    AIRY_FREE(m->description);
    AIRY_FREE(m->executable);
    if (m->params) {
        for (size_t i = 0; i < m->param_count; ++i) {
            AIRY_FREE((void *)m->params[i].name);
            AIRY_FREE((void *)m->params[i].schema);
        }
        AIRY_FREE(m->params);
    }
    AIRY_FREE(m->permission_rule);
    AIRY_MEMSET(m, 0, sizeof(*m));
}

static pool_job_t *job_new(const tool_metadata_t *meta, const char *params_json,
                           const char *agent_id)
{
    pool_job_t *job = (pool_job_t *)AIRY_CALLOC(1, sizeof(pool_job_t));
    if (!job)
        return NULL;

    job->meta.id = dup_str(meta->id);
    job->meta.name = dup_str(meta->name);
    job->meta.description = dup_str(meta->description);
    job->meta.executable = dup_str(meta->executable);
    job->meta.params = params_deep_copy(meta->params, meta->param_count);
    job->meta.param_count = meta->param_count;
    job->meta.timeout_sec = meta->timeout_sec;
    job->meta.cacheable = meta->cacheable;
    job->meta.access = meta->access;
    job->meta.permission_rule = dup_str(meta->permission_rule);
    if (!job->meta.id) {
        meta_copy_free(&job->meta);
        AIRY_FREE(job);
        return NULL;
    }

    job->params_json = dup_str(params_json);
    job->agent_id = dup_str(agent_id);
    return job;
}

static void job_free(pool_job_t *job)
{
    if (!job)
        return;
    meta_copy_free(&job->meta);
    AIRY_FREE(job->params_json);
    AIRY_FREE(job->agent_id);
    if (job->detached)
        tool_result_free(job->result); /* detach：真实结果丢弃 */
    AIRY_FREE(job);
}

static tool_result_t *synth_cancel_result(void)
{
    tool_result_t *res = (tool_result_t *)AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!res)
        return NULL;
    res->success = 0;
    res->output = AIRY_STRDUP("");
    res->error = AIRY_STRDUP("Tool execution canceled (deadline exceeded)");
    res->exit_code = -1;
    res->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
    return res;
}

/* ---------- 等待预算 ---------- */

static uint64_t wait_budget_ms(const executor_pool_t *p, const tool_metadata_t *meta)
{
    const char *ov = getenv(ENV_WAIT_BUDGET);
    if (ov && ov[0]) {
        long v = strtol(ov, NULL, 10);
        if (v > 0)
            return (uint64_t)v;
    }
    /* 与执行面同源：executor_budget_ms 是唯一 deadline 所有者。
     * 此前这里按 max(meta, 默认) 等待而执行面只取默认值，meta 小于默认值时
     * 进程会活过等待预算——取消永不在工具自然结束前生效。改为同一函数后，
     * slack 只需覆盖 SIGKILL 后的 drain/reap 窗口。 */
    uint64_t budget = (uint64_t)executor_budget_ms(p->exec, meta) + WAIT_SLACK_MS;
    /* 交互审批等待发生在 worker 内：预算须覆盖其上限，避免误报取消 */
    if (tool_executor_interactive_enabled(p->exec))
        budget += approval_timeout_ms();
    return budget;
}

/* ---------- worker ---------- */

static void *pool_worker(void *arg)
{
    executor_pool_t *p = (executor_pool_t *)arg;

    for (;;) {
        airy_mtx_lock(&p->lock);
        while (!p->shutdown && p->count == 0)
            airy_cond_wait(&p->work_cond, &p->lock);
        if (p->count == 0) {
            airy_mtx_unlock(&p->lock);
            break; /* shutdown 且无残留 job */
        }
        pool_job_t *job = p->queue[p->head];
        p->head = (p->head + 1) % POOL_QUEUE_CAP;
        p->count--;
        p->running++;
        airy_mtx_unlock(&p->lock);

        tool_result_t *res = NULL;
        int ret = tool_executor_run(p->exec, &job->meta, job->params_json, job->agent_id, &res);

        airy_mtx_lock(&p->lock);
        job->ret = ret;
        job->result = res;
        job->done = 1;
        p->running--;
        int detach = job->detached;
        airy_cond_broadcast(&p->done_cond);
        airy_mtx_unlock(&p->lock);

        if (detach)
            job_free(job);
    }
    return NULL;
}

/* ---------- 池生命周期 ---------- */

executor_pool_t *executor_pool_new(tool_executor_t *exec)
{
    if (!exec)
        return NULL;

    /* 来源优先级：env AIRY_TOOL_EXEC_WORKERS > executor 配置 > 内置默认 2。
     * 配置面此前是死字段（无消费点），此处接线后 service 可用它表达部署偏好。 */
    int workers = executor_max_workers(exec);
    if (workers < 1 || workers > POOL_WORKER_MAX)
        workers = POOL_WORKER_DEFAULT;
    const char *ws = getenv(ENV_EXEC_WORKERS);
    if (ws && ws[0]) {
        long v = strtol(ws, NULL, 10);
        if (v >= 1 && v <= POOL_WORKER_MAX)
            workers = (int)v;
    }

    executor_pool_t *p = (executor_pool_t *)AIRY_CALLOC(1, sizeof(executor_pool_t));
    if (!p)
        return NULL;
    p->exec = exec;
    p->worker_count = workers;
    if (airy_mtx_init(&p->lock) != 0) {
        AIRY_FREE(p);
        return NULL;
    }
    if (airy_cond_init(&p->work_cond) != 0 || airy_cond_init(&p->done_cond) != 0) {
        airy_cond_destroy(&p->work_cond);
        airy_cond_destroy(&p->done_cond);
        airy_mtx_destroy(&p->lock);
        AIRY_FREE(p);
        return NULL;
    }

    p->threads = (airy_thread_t *)AIRY_CALLOC((size_t)workers,
                                              sizeof(airy_thread_t));
    if (!p->threads) {
        executor_pool_free(p);
        return NULL;
    }
    int started = 0;
    for (; started < workers; ++started) {
        if (airy_platform_thread_create(&p->threads[started], pool_worker, p) != 0)
            break;
    }
    if (started == 0) {
        /* 无任何 worker：关闭标志后走统一销毁路径（残留队列此时为空） */
        airy_mtx_lock(&p->lock);
        p->shutdown = 1;
        airy_mtx_unlock(&p->lock);
        AIRY_FREE(p->threads);
        p->threads = NULL;
        p->worker_count = 0;
        executor_pool_free(p);
        return NULL;
    }
    p->worker_count = started;

    SVC_LOG_INFO("executor_pool: started with %d worker(s), queue cap %d", started,
                 POOL_QUEUE_CAP);
    return p;
}

void executor_pool_free(executor_pool_t *p)
{
    if (!p)
        return;

    airy_mtx_lock(&p->lock);
    p->shutdown = 1;
    /* 残留队列 job：置 done + 合成取消结果，唤醒等待方按正常完成路径取走 */
    for (int i = 0; i < p->count; ++i) {
        pool_job_t *job = p->queue[(p->head + i) % POOL_QUEUE_CAP];
        if (!job->done) {
            job->ret = AIRY_ERR_CANCELED;
            job->result = synth_cancel_result();
            job->done = 1;
        }
    }
    p->head = p->tail = p->count = 0;
    /* 等待在途 job 全部落定（worker 完成时会 broadcast），避免 UAF/悬挂 */
    while (p->running > 0)
        airy_cond_timedwait(&p->done_cond, &p->lock, WAIT_SLICE_MS);
    airy_cond_broadcast(&p->work_cond);
    airy_cond_broadcast(&p->done_cond);
    airy_mtx_unlock(&p->lock);

    for (int i = 0; i < p->worker_count; ++i)
        airy_platform_thread_join(p->threads[i], NULL);
    AIRY_FREE(p->threads);

    airy_cond_destroy(&p->work_cond);
    airy_cond_destroy(&p->done_cond);
    airy_mtx_destroy(&p->lock);
    AIRY_FREE(p);
}

/* ---------- 提交 + 预算等待 ---------- */

int executor_pool_run(executor_pool_t *p, const tool_metadata_t *meta, const char *params_json,
                      const char *agent_id, tool_result_t **out_result)
{
    if (!p || !meta || !out_result)
        return AIRY_ERR_INVALID_PARAM;
    *out_result = NULL;

    uint64_t budget = wait_budget_ms(p, meta);
    pool_job_t *job = job_new(meta, params_json, agent_id);
    if (!job)
        return AIRY_ERR_OUT_OF_MEMORY;

    airy_mtx_lock(&p->lock);
    if (p->shutdown) {
        airy_mtx_unlock(&p->lock);
        job_free(job);
        return AIRY_ERR_CANCELED;
    }
    if (p->count >= POOL_QUEUE_CAP) {
        airy_mtx_unlock(&p->lock);
        SVC_LOG_WARN("executor_pool: queue full (%d) — backpressure reject for '%s'", p->count,
                     meta->id ? meta->id : "?");
        job_free(job);
        return AIRY_ERR_BUSY;
    }
    p->queue[p->tail] = job;
    p->tail = (p->tail + 1) % POOL_QUEUE_CAP;
    p->count++;
    airy_cond_signal(&p->work_cond);

    /* 预算按单调钟实测：cond_timedwait 允许超时过睡（POSIX 语义，
     * macOS 定时器合并 + CI 负载下 100ms 切片可实睡 135ms+），按切片
     * 值累加会系统性低估真实流逝时间，预算晚于工具自然完成才生效
     * （取消永不触发）。切片只作唤醒粒度，deadline 以 airy_time_ms
     * 兜底判定。 */
    uint64_t deadline = airy_time_ms() + budget;
    int canceled = 0;
    while (!job->done) {
        uint64_t now = airy_time_ms();
        if (now >= deadline) {
            job->detached = 1;
            canceled = 1;
            break;
        }
        uint64_t remain = deadline - now;
        uint32_t slice = (remain > WAIT_SLICE_MS) ? WAIT_SLICE_MS : (uint32_t)remain;
        airy_cond_timedwait(&p->done_cond, &p->lock, slice);
    }

    if (canceled) {
        airy_mtx_unlock(&p->lock);
        SVC_LOG_WARN("executor_pool: '%s' still running after budget %llu ms — reporting canceled, "
                     "job detached (tool did not honor its deadline)",
                     meta->id ? meta->id : "?", (unsigned long long)budget);
        *out_result = synth_cancel_result();
        return AIRY_ERR_CANCELED;
    }

    int ret = job->ret;
    *out_result = job->result; /* 归属移交给调用方 */
    airy_mtx_unlock(&p->lock);
    job->detached = 0; /* 非 detach 完成：result 已移交，仅释放 job 壳 */
    job_free(job);
    return ret;
}
