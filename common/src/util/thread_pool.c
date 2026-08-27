// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "thread_pool.h"

#include "error.h"
#include "airy_memory.h"
#include "svc_logger.h"

#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct task_node {
    thread_task_fn_t fn;
    void *arg;
    struct task_node *next;
} task_node_t;

struct thread_pool_s {
    thread_pool_config_t config;
    airy_thread_t *threads;
    uint32_t thread_count;
    task_node_t *queue_head;
    task_node_t *queue_tail;
    uint32_t queue_count;
    uint32_t active_count;
    airy_mtx_t lock;
    airy_cond_t notify;
    bool running;
    bool shutdown;
};

static void *worker_thread_func(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;

    while (true) {
        airy_mtx_lock(&pool->lock);

        while (pool->queue_count == 0 && !pool->shutdown) {
            airy_cond_wait(&pool->notify, &pool->lock);
        }

        if (pool->shutdown && pool->queue_count == 0) {
            airy_mtx_unlock(&pool->lock);
            break;
        }

        task_node_t *task = pool->queue_head;
        if (task) {
            pool->queue_head = task->next;
            if (!pool->queue_head)
                pool->queue_tail = NULL;
            pool->queue_count--;
            pool->active_count++;
        }

        airy_mtx_unlock(&pool->lock);

        if (task) {
            task->fn(task->arg);
            AIRY_FREE(task);

            airy_mtx_lock(&pool->lock);
            pool->active_count--;
            airy_mtx_unlock(&pool->lock);
        }
    }

    return NULL;
}

thread_pool_t *thread_pool_create(const thread_pool_config_t *config)
{
    thread_pool_t *pool = (thread_pool_t *)AIRY_CALLOC(1, sizeof(thread_pool_t));
    if (!pool) {
        SVC_LOG_ERROR("thread_pool_create: memory allocation failed for pool");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (config) {
        pool->config = *config;
    } else {
        thread_pool_config_t defaults;
        defaults.min_threads = 2;
        defaults.max_threads = 8;
        defaults.queue_size = 256;
        defaults.idle_timeout_ms = 30000;
        pool->config = defaults;
    }

    pool->threads = (airy_thread_t *)AIRY_CALLOC(pool->config.max_threads, sizeof(airy_thread_t));
    if (!pool->threads) {
        SVC_LOG_ERROR(
            "thread_pool_create: memory allocation failed for threads array (max_threads=%u)",
            pool->config.max_threads);
        AIRY_FREE(pool);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_init(&pool->lock);
    airy_cond_init(&pool->notify);

    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_count = 0;
    pool->active_count = 0;
    pool->running = true;
    pool->shutdown = false;

    uint32_t num_threads = pool->config.min_threads;
    if (num_threads < 1)
        num_threads = 1;
    if (num_threads > pool->config.max_threads)
        num_threads = pool->config.max_threads;

    for (uint32_t i = 0; i < num_threads; i++) {
        int rc = airy_thread_create(&pool->threads[i], worker_thread_func, pool);
        if (rc == 0) {
            pool->thread_count++;
        } else {
            SVC_LOG_WARN("thread_pool_create: thread creation failed index=%u rc=%d", i, rc);
        }
    }

    if (pool->thread_count == 0) {
        SVC_LOG_ERROR("thread_pool_create: all thread creations failed (attempted=%u)",
                      num_threads);
        airy_mtx_destroy(&pool->lock);
        airy_cond_destroy(&pool->notify);
        AIRY_FREE(pool->threads);
        AIRY_FREE(pool);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    return pool;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    if (!pool)
        return;

    airy_mtx_lock(&pool->lock);
    pool->shutdown = true;
    airy_cond_broadcast(&pool->notify);
    airy_mtx_unlock(&pool->lock);

    for (uint32_t i = 0; i < pool->thread_count; i++) {
        airy_thread_join(pool->threads[i], NULL);
    }

    task_node_t *node = pool->queue_head;
    while (node) {
        task_node_t *next = node->next;
        AIRY_FREE(node);
        node = next;
    }

    airy_mtx_destroy(&pool->lock);
    airy_cond_destroy(&pool->notify);
    AIRY_FREE(pool->threads);
    AIRY_FREE(pool);
}

int thread_pool_submit(thread_pool_t *pool, thread_task_fn_t task, void *arg)
{
    if (!pool || !task) {
        SVC_LOG_ERROR("thread_pool_submit: null parameter pool=%p task=%p", (void *)pool,
                      (void *)(uintptr_t)task);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!pool->running) {
        SVC_LOG_ERROR("thread_pool_submit: pool not running");
        return AIRY_ERR_UNKNOWN;
    }

    task_node_t *node = (task_node_t *)AIRY_CALLOC(1, sizeof(task_node_t));
    if (!node) {
        SVC_LOG_ERROR("thread_pool_submit: memory allocation failed for task node");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    node->fn = task;
    node->arg = arg;
    node->next = NULL;

    airy_mtx_lock(&pool->lock);

    if (pool->shutdown) {
        airy_mtx_unlock(&pool->lock);
        AIRY_FREE(node);
        SVC_LOG_WARN("thread_pool_submit: pool is shutting down");
        return AIRY_ERR_UNKNOWN;
    }

    if (pool->queue_count >= pool->config.queue_size) {
        airy_mtx_unlock(&pool->lock);
        AIRY_FREE(node);
        SVC_LOG_WARN("thread_pool_submit: queue full queue_count=%u queue_size=%u",
                     pool->queue_count, pool->config.queue_size);
        return AIRY_ERR_OVERFLOW;
    }

    if (pool->queue_tail) {
        pool->queue_tail->next = node;
    } else {
        pool->queue_head = node;
    }
    pool->queue_tail = node;
    pool->queue_count++;

    airy_cond_signal(&pool->notify);
    airy_mtx_unlock(&pool->lock);

    return 0;
}

uint32_t thread_pool_active_count(thread_pool_t *pool)
{
    if (!pool) {
        SVC_LOG_ERROR("thread_pool_active_count: null pool parameter");
        return 0;
    }
    airy_mtx_lock(&pool->lock);
    uint32_t count = pool->active_count;
    airy_mtx_unlock(&pool->lock);
    return count;
}

uint32_t thread_pool_pending_count(thread_pool_t *pool)
{
    if (!pool) {
        SVC_LOG_ERROR("thread_pool_pending_count: null pool parameter");
        return 0;
    }
    airy_mtx_lock(&pool->lock);
    uint32_t count = pool->queue_count;
    airy_mtx_unlock(&pool->lock);
    return count;
}

bool thread_pool_is_running(thread_pool_t *pool)
{
    if (!pool) {
        SVC_LOG_ERROR("thread_pool_is_running: null pool parameter");
        return false;
    }
    return pool->running;
}
