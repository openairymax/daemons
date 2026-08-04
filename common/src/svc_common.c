// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_common.c
 * @brief 服务公共实现 - 统一服务管理框架
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 实现 svc_common.h 中定义的统一服务管理接口。
 * 本模块为 AgentRT daemon 模块提供标准化的服务生命周期管理、
 * 状态监控、健康检查和统计收集功能。
 *
 * 设计原则：
 * 1. 统一的服务接口定义（K-2 接口契约化）
 * 2. 明确的生命周期管理
 * 3. 标准化的错误处理（E-6 错误可追溯）
 * 4. 线程安全的实现（E-5 并发安全）
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see Service_Management_Framework_Design.md
 */

#include "svc_common.h"
/* P0.17 阶段 4: 显式包含 daemon_errors.h 提供 DAEMON_E* 扩展错误码。
 * commons 版 svc_common.h（经 -I 路径优先解析）不包含 daemon_errors.h，
 * daemons 内部源文件需显式包含以获取 DAEMON_EINIT/ESTATE/EHEALTH 等别名。 */
#include "daemon_errors.h"

#include "atomic_compat.h"
#include "error.h"
#include "ipc_client.h"
#include "airy_memory.h"
#include "memory_stats_reporter.h"
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"
#include "thread_pool.h"

#ifndef _WIN32
/* SIGALRM/alarm/sigaction 仅 POSIX 可用；Windows 分支不依赖 signal.h
 *（force-stop 超时机制在 Windows 退化为告警，见 airy_svc_stop） */
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef AIRY_HAS_CURL
#include <curl/curl.h>
#endif

/* ==================== 内部常量 ==================== */

#define MAX_SERVICE_NAME_LEN 64
#define MAX_SERVICE_VERSION_LEN 32
#define MAX_SERVICES 256
#define DEFAULT_HEALTHCHECK_INTERVAL_MS 5000 /* 5秒健康检查间隔 */

/* ==================== 内部数据结构 ==================== */

/**
 * @brief 服务实例内部结构
 */
typedef struct airy_svc_internal {
    /* 基本信息 */
    char name[MAX_SERVICE_NAME_LEN];
    char version[MAX_SERVICE_VERSION_LEN];

    /* 状态管理 */
    airy_svc_state_t state;
    airy_mtx_t state_mutex;

    /* 配置 */
    airy_svc_config_t config;
    uint32_t capabilities;

    /* 统计信息 */
    airy_svc_stats_t stats;
    airy_mtx_t stats_mutex;

    /* 接口 */
    airy_svc_interface_t iface;

    /* 健康检查状态 */
    uint64_t last_healthcheck_time;
    int healthcheck_failures;

    /* 用户上下文数据 */
    void *user_data;

    /* 并发支持 */
    void *thread_pool;
    pthread_t *threads;
    size_t thread_count;

    /* 链表支持 */
    struct airy_svc_internal *next;
} airy_svc_internal_t;

/**
 * @brief 服务注册表内部状态
 */
static struct {
    airy_svc_internal_t *services; /* 服务链表头 */
    airy_mtx_t registry_mutex;       /* 注册表互斥锁 */
    uint32_t service_count;               /* 当前服务数 */
    int initialized;                      /* 模块初始化标志 */
} g_registry = {.services = NULL, .service_count = 0, .initialized = 0};

/* ==================== 辅助函数 ==================== */

/**
 * @brief 初始化服务管理模块
 */
static airy_err_t svc_common_module_init(void)
{
    if (g_registry.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = AIRY_SUCCESS;

    /* 初始化注册表互斥锁 */
    err = airy_mtx_init(&g_registry.registry_mutex);
    if (err != AIRY_SUCCESS) {
        LOG_ERROR("Failed to initialize registry mutex: %d", err);
        AIRY_ERROR(DAEMON_EINIT, "svc_common: registry mutex init failed");
    }

    g_registry.initialized = 1;

    /* Initialize memory stats reporter (SEC-15) */
    airy_mem_stats_reporter_init();

    LOG_DEBUG("Service common module initialized");

    return AIRY_SUCCESS;
}

/**
 * @brief 清理服务管理模块
 */
static void svc_common_module_cleanup(void)
{
    if (!g_registry.initialized) {
        return;
    }

    /* 注意：不在这里销毁服务，应由调用者负责 */
    airy_mtx_destroy(&g_registry.registry_mutex);
    g_registry.initialized = 0;

    LOG_DEBUG("Service common module cleaned up");
}

/**
 * @brief 查找服务内部结构
 */
static airy_svc_internal_t *find_service_internal(const char *name)
{
    if (!name || !g_registry.initialized) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *current = g_registry.services;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }

    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/**
 * @brief 注册服务到内部注册表
 */
static airy_err_t register_service_internal(airy_svc_internal_t *service)
{
    if (!service || !g_registry.initialized) {
        AIRY_ERROR(AIRY_EINVAL, "register_service_internal: null service");
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    /* 检查服务是否已存在 */
    if (find_service_internal(service->name)) {
        airy_mtx_unlock(&g_registry.registry_mutex);
        return AIRY_EEXIST;
    }

    /* 添加到链表头部 */
    service->next = g_registry.services;
    g_registry.services = service;
    g_registry.service_count++;

    airy_mtx_unlock(&g_registry.registry_mutex);

    LOG_INFO("Service '%s' registered internally", service->name);

    return AIRY_SUCCESS;
}

/**
 * @brief 从内部注册表注销服务
 */
static airy_err_t unregister_service_internal(airy_svc_internal_t *service)
{
    if (!service || !g_registry.initialized) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    /* 查找服务并移除 */
    airy_svc_internal_t **prev = &g_registry.services;
    airy_svc_internal_t *current = g_registry.services;

    while (current) {
        if (current == service) {
            *prev = current->next;
            g_registry.service_count--;

            airy_mtx_unlock(&g_registry.registry_mutex);
            LOG_INFO("Service '%s' unregistered internally", service->name);
            return AIRY_SUCCESS;
        }

        prev = &current->next;
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);

    return AIRY_ENOENT; /* 服务未找到 */
}

/**
 * @brief 更新服务统计信息
 */
static void __attribute__((unused)) update_service_stats(airy_svc_internal_t *service,
                                                         bool success, uint64_t process_time_ms)
{
    if (!service) {
        return;
    }

    airy_mtx_lock(&service->stats_mutex);

    service->stats.request_count++;

    if (success) {
        service->stats.success_count++;
    } else {
        service->stats.error_count++;
    }

    service->stats.total_time_ms += process_time_ms;

    if (process_time_ms > service->stats.max_time_ms) {
        service->stats.max_time_ms = process_time_ms;
    }

    if (service->stats.min_time_ms == 0 || process_time_ms < service->stats.min_time_ms) {
        service->stats.min_time_ms = process_time_ms;
    }

    if (service->stats.request_count > 0) {
        service->stats.avg_time_ms =
            (double)service->stats.total_time_ms / service->stats.request_count;
    }

    airy_mtx_unlock(&service->stats_mutex);
}

/* ==================== 公共API实现 ==================== */

/* 服务生命周期管理 */

airy_err_t airy_svc_create(airy_svc_t *out_service, const char *name,
                                       const airy_svc_interface_t *iface,
                                       const airy_svc_config_t *config)
{

    if (!out_service || !name || !iface || !config) {
        return AIRY_EINVAL;
    }

    /* 初始化模块（如果未初始化） */
    airy_err_t err = svc_common_module_init();
    if (err != AIRY_SUCCESS) {
        return err;
    }

    /* 检查名称长度 */
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= MAX_SERVICE_NAME_LEN) {
        return AIRY_EINVAL;
    }

    /* 分配服务结构 */
    airy_svc_internal_t *service =
        (airy_svc_internal_t *)AIRY_CALLOC(1, sizeof(airy_svc_internal_t));
    if (!service) {
        AIRY_ERROR(AIRY_ENOMEM, "airy_svc_create: calloc service failed");
    }

    /* 初始化基本信息 */
    if (safe_strcpy(service->name, name, MAX_SERVICE_NAME_LEN) != 0) {
        AIRY_FREE(service);
        AIRY_ERROR(AIRY_EINVAL, "airy_svc_create: name copy failed");
    }

    if (config->version) {
        if (safe_strcpy(service->version, config->version, MAX_SERVICE_VERSION_LEN) != 0) {
            AIRY_FREE(service);
            AIRY_ERROR(AIRY_EINVAL, "airy_svc_create: version copy failed");
        }
    }

    /* 初始化状态 */
    service->state = AIRY_SVC_STATE_CREATED;
    err = airy_mtx_init(&service->state_mutex);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(service);
        AIRY_ERROR(err, "airy_svc_create: state mutex init failed");
    }

    /* 初始化统计互斥锁 */
    err = airy_mtx_init(&service->stats_mutex);
    if (err != AIRY_SUCCESS) {
        airy_mtx_destroy(&service->state_mutex);
        AIRY_FREE(service);
        return err;
    }

    /* 复制配置 */
    __builtin_memcpy(&service->config, config, sizeof(airy_svc_config_t));
    service->capabilities = config->capabilities;

    /* 复制接口 */
    __builtin_memcpy(&service->iface, iface, sizeof(airy_svc_interface_t));

    /* 初始化线程追踪 */
    service->threads = NULL;
    service->thread_count = 0;

    /* 初始化统计信息 */
    __builtin_memset(&service->stats, 0, sizeof(airy_svc_stats_t));

    /* 注册到内部注册表 */
    err = register_service_internal(service);
    if (err != AIRY_SUCCESS) {
        airy_mtx_destroy(&service->stats_mutex);
        airy_mtx_destroy(&service->state_mutex);
        AIRY_FREE(service);
        return err;
    }

    *out_service = (airy_svc_t)service;

    LOG_INFO("Service '%s' created successfully", name);

    return AIRY_SUCCESS;
}

void airy_svc_destroy(airy_svc_t svc)
{
    if (!svc) {
        return;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    /* 如果服务还在运行，先强制停止（带5秒超时保护） */
    if (service->state == AIRY_SVC_STATE_RUNNING || service->state == AIRY_SVC_STATE_PAUSED) {
        airy_svc_stop((airy_svc_t)service, true);
    }

    /* 从注册表注销（即使部分资源已损坏，继续清理） */
    {
        airy_err_t unreg_err = unregister_service_internal(service);
        if (unreg_err != AIRY_SUCCESS && unreg_err != AIRY_ENOENT) {
            LOG_WARN("Service '%s' unregister during destroy returned %d - continuing cleanup",
                     service->name, unreg_err);
        }
    }

    /* 调用服务的销毁函数（容错：失败不阻塞后续清理） */
    if (service->iface.destroy) {
        service->iface.destroy(svc);
    }

    /* 清理资源：跳过已损坏/已释放的资源 */
    airy_mtx_destroy(&service->state_mutex);
    airy_mtx_destroy(&service->stats_mutex);

    /* 释放内存：无论前面是否出错，内存必须释放 */
    if (service->threads) {
        AIRY_FREE(service->threads);
        service->threads = NULL;
        service->thread_count = 0;
    }
    AIRY_FREE(service);

    LOG_INFO("Service destroyed");
}

airy_err_t airy_svc_init(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    /* 状态检查 */
    if (service->state != AIRY_SVC_STATE_CREATED) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_ERROR("Service '%s' cannot initialize from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    /* 更新状态 */
    service->state = AIRY_SVC_STATE_INITIALIZING;
    airy_mtx_unlock(&service->state_mutex);

    /* 调用服务的初始化函数 */
    airy_err_t err = AIRY_SUCCESS;
    if (service->iface.init) {
        err = service->iface.init(svc, &service->config);
    }

    airy_mtx_lock(&service->state_mutex);
    if (err == AIRY_SUCCESS) {
        service->state = AIRY_SVC_STATE_READY;
        LOG_INFO("Service '%s' initialized successfully", service->name);
    } else {
        service->state = AIRY_SVC_STATE_ERROR;
        LOG_ERROR("Service '%s' initialization failed: %d", service->name, err);
    }

    airy_mtx_unlock(&service->state_mutex);

    return err;
}

airy_err_t airy_svc_start(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    /* 状态检查 */
    if (service->state != AIRY_SVC_STATE_READY && service->state != AIRY_SVC_STATE_STOPPED &&
        service->state != AIRY_SVC_STATE_PAUSED && service->state != AIRY_SVC_STATE_ZOMBIE) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_ERROR("Service '%s' cannot start from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    /* 更新状态 */
    airy_svc_state_t old_state = service->state;
    service->state = AIRY_SVC_STATE_RUNNING;
    airy_mtx_unlock(&service->state_mutex);

    /* 调用服务的启动函数 */
    airy_err_t err = AIRY_SUCCESS;
    if (service->iface.start) {
        err = service->iface.start(svc);
    }

    if (err != AIRY_SUCCESS) {
        airy_mtx_lock(&service->state_mutex);
        service->state = old_state; /* 恢复原状态 */
        airy_mtx_unlock(&service->state_mutex);

        LOG_ERROR("Service '%s' start failed: %d", service->name, err);
        return err;
    }

    LOG_INFO("Service '%s' started successfully", service->name);

    return AIRY_SUCCESS;
}

#ifndef _WIN32
/* force-stop 超时机制依赖 SIGALRM/alarm，仅 POSIX 可用。
 * Windows 分支不引用这些符号（见 airy_svc_stop 的 _WIN32 分支）。 */
#define FORCE_STOP_TIMEOUT_SEC 5

static volatile sig_atomic_t g_svc_stop_timeout_flag = 0;

static void svc_stop_timeout_handler(int signum __attribute__((unused)))
{
    g_svc_stop_timeout_flag = 1;
}
#endif /* !_WIN32 */

airy_err_t airy_svc_stop(airy_svc_t svc, bool force)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    /* 状态检查 */
    if (service->state != AIRY_SVC_STATE_RUNNING && service->state != AIRY_SVC_STATE_PAUSED) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_WARN("Service '%s' cannot stop from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    /* 更新状态 */
    service->state = AIRY_SVC_STATE_STOPPING;
    airy_mtx_unlock(&service->state_mutex);

    /* 调用服务的停止函数 */
    airy_err_t err = AIRY_SUCCESS;
    bool zombie = false;

    if (service->iface.stop) {
#ifndef _WIN32
        struct sigaction old_act, new_act;
        if (force) {
            g_svc_stop_timeout_flag = 0;
            __builtin_memset(&new_act, 0, sizeof(new_act));
            new_act.sa_handler = svc_stop_timeout_handler;
            sigemptyset(&new_act.sa_mask);
            new_act.sa_flags = SA_RESETHAND;
            sigaction(SIGALRM, &new_act, &old_act);
            alarm(FORCE_STOP_TIMEOUT_SEC);
        }

        err = service->iface.stop(svc, force);

        if (force) {
            alarm(0);
            sigaction(SIGALRM, &old_act, NULL);
            if (g_svc_stop_timeout_flag) {
                zombie = true;
                LOG_ERROR("Service '%s' force stop timed out after %d seconds - marking ZOMBIE",
                          service->name, FORCE_STOP_TIMEOUT_SEC);

#ifdef AIRY_OS_UNIX
                if (service->threads && service->thread_count > 0) {
                    LOG_WARN("Service '%s' attempting deadlock recovery for %d threads",
                             service->name, (int)service->thread_count);
                    for (size_t i = 0; i < service->thread_count; i++) {
                        pthread_t tid = service->threads[i];
                        if (tid) {
                            void *retval = NULL;
                            int join_rc = pthread_tryjoin_np(tid, &retval);
                            if (join_rc == EBUSY) {
                                LOG_ERROR("Service '%s' thread[%zu] deadlocked - cancelling",
                                          service->name, i);
                                pthread_cancel(tid);
                                pthread_join(tid, NULL);
                            } else if (join_rc == 0) {
                                LOG_INFO("Service '%s' thread[%zu] joined with retval=%p",
                                         service->name, i, retval);
                            }
                        }
                    }
                }
#endif
            }
        }
#else  /* _WIN32 */
        /* Windows 无 SIGALRM/alarm/sigaction 机制，无法对 force stop 施加超时
         * 强杀。退化语义：直接调用服务的 stop；超时保护不可用，若 stop 阻塞
         * 将挂起该调用。明确告警以避免静默退化（ARCHITECTURAL_PRINCIPLES E-6
         * 错误可追溯）。正常（非 force）停机路径不受影响。 */
        if (force) {
            LOG_WARN("Service '%s' force stop timeout not supported on Windows "
                     "(SIGALRM/alarm unavailable) - force stop may hang",
                     service->name);
        }
        err = service->iface.stop(svc, force);
#endif /* _WIN32 */
    }

    airy_mtx_lock(&service->state_mutex);
    if (err == AIRY_SUCCESS || force) {
        service->state = zombie ? AIRY_SVC_STATE_ZOMBIE : AIRY_SVC_STATE_STOPPED;
        LOG_INFO("Service '%s' stopped %s", service->name,
                 force ? (zombie ? "(ZOMBIE)" : "(forced)") : "gracefully");
    } else {
        service->state = AIRY_SVC_STATE_ERROR;
        LOG_ERROR("Service '%s' stop failed: %d", service->name, err);
    }

    airy_mtx_unlock(&service->state_mutex);

    return err;
}

typedef struct {
    airy_svc_t service;
    char *method;
    char *params_json;
    airy_svc_async_complete_fn on_complete;
    void *user_data;
} async_request_context_t;

static void async_request_worker(void *arg)
{
    async_request_context_t *ctx = (async_request_context_t *)arg;
    if (!ctx)
        return;

    char *response_json = NULL;
    airy_err_t err = AIRY_EINVAL;

    airy_svc_internal_t *svc = (airy_svc_internal_t *)ctx->service;
    if (svc && svc->iface.handle_request) {
        err = svc->iface.handle_request(ctx->service, ctx->method, ctx->params_json, &response_json,
                                        svc->user_data);
    }

    if (ctx->on_complete) {
        ctx->on_complete(ctx->service, ctx->method, err, response_json, ctx->user_data);
    } else {
        if (response_json)
            AIRY_FREE(response_json);
    }

    AIRY_FREE(ctx->method);
    AIRY_FREE(ctx->params_json);
    AIRY_FREE(ctx);
}

airy_err_t airy_svc_set_thread_pool(airy_svc_t svc, void *pool)
{
    if (!svc)
        return AIRY_EINVAL;
    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    service->thread_pool = pool;
    return AIRY_SUCCESS;
}

int airy_svc_handle_request_async(airy_svc_t service, const char *method,
                                         const char *params_json,
                                         airy_svc_async_complete_fn on_complete, void *user_data)
{
    if (!service || !method) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "handle_request_async: null service or method");
    }

    airy_svc_internal_t *svc = (airy_svc_internal_t *)service;

    async_request_context_t *ctx = (async_request_context_t *)AIRY_CALLOC(1, sizeof(*ctx));
    if (!ctx) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "handle_request_async: calloc ctx failed");
    }

    ctx->service = service;
    ctx->method = AIRY_STRDUP(method);
    ctx->params_json = params_json ? AIRY_STRDUP(params_json) : NULL;
    ctx->on_complete = on_complete;
    ctx->user_data = user_data;

    if (svc->thread_pool) {
        int rc = thread_pool_submit(svc->thread_pool, async_request_worker, ctx);
        if (rc != 0) {
            AIRY_FREE(ctx->method);
            AIRY_FREE(ctx->params_json);
            AIRY_FREE(ctx);
            return rc;
        }
        return 0;
    }

    async_request_worker(ctx);
    return 0;
}

airy_err_t airy_svc_pause(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    /* 状态检查 */
    if (service->state != AIRY_SVC_STATE_RUNNING) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_ERROR("Service '%s' cannot pause from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    /* 检查是否支持暂停 */
    if (!(service->capabilities & AIRY_SVC_CAP_PAUSEABLE)) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_ERROR("Service '%s' does not support pause", service->name);
        return AIRY_EPROTONOSUPPORT;
    }

    /* 更新状态 */
    service->state = AIRY_SVC_STATE_PAUSED;
    airy_mtx_unlock(&service->state_mutex);

    LOG_INFO("Service '%s' paused", service->name);

    return AIRY_SUCCESS;
}

airy_err_t airy_svc_resume(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    /* 状态检查 */
    if (service->state != AIRY_SVC_STATE_PAUSED) {
        airy_mtx_unlock(&service->state_mutex);
        LOG_ERROR("Service '%s' cannot resume from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    /* 更新状态 */
    service->state = AIRY_SVC_STATE_RUNNING;
    airy_mtx_unlock(&service->state_mutex);

    LOG_INFO("Service '%s' resumed", service->name);

    return AIRY_SUCCESS;
}

/* 服务状态查询 */

airy_svc_state_t airy_svc_get_state(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_SVC_STATE_NONE;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);
    airy_svc_state_t state = service->state;
    airy_mtx_unlock(&service->state_mutex);

    return state;
}

bool airy_svc_is_ready(airy_svc_t svc)
{
    airy_svc_state_t state = airy_svc_get_state(svc);
    return state == AIRY_SVC_STATE_READY;
}

bool airy_svc_is_running(airy_svc_t svc)
{
    airy_svc_state_t state = airy_svc_get_state(svc);
    return state == AIRY_SVC_STATE_RUNNING;
}

const char *airy_svc_get_name(airy_svc_t svc)
{
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return service->name;
}

const char *airy_svc_get_version(airy_svc_t svc)
{
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return service->version[0] ? service->version : "0.1.1";
}

/* 服务统计 */

airy_err_t airy_svc_get_stats(airy_svc_t svc, airy_svc_stats_t *out_stats)
{

    if (!svc || !out_stats) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->stats_mutex);
    __builtin_memcpy(out_stats, &service->stats, sizeof(airy_svc_stats_t));
    airy_mtx_unlock(&service->stats_mutex);

    return AIRY_SUCCESS;
}

void airy_svc_reset_stats(airy_svc_t svc)
{
    if (!svc) {
        return;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->stats_mutex);
    __builtin_memset(&service->stats, 0, sizeof(airy_svc_stats_t));
    airy_mtx_unlock(&service->stats_mutex);

    LOG_DEBUG("Service '%s' stats reset", service->name);
}

/* 服务健康检查 */

airy_err_t airy_svc_healthcheck(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    /* 如果服务提供了健康检查函数，则使用它 */
    if (service->iface.healthcheck) {
        airy_err_t err = service->iface.healthcheck(svc);

        /* 更新健康检查状态 */
        uint64_t current_time = airy_time_ms();
        service->last_healthcheck_time = current_time;

        if (err != AIRY_SUCCESS) {
            service->healthcheck_failures++;
            LOG_WARN("Service '%s' health check failed: %d (failures: %d)", service->name, err,
                     service->healthcheck_failures);
        } else {
            service->healthcheck_failures = 0;
        }

        return err;
    }

    /* 默认健康检查：检查服务状态 */
    airy_svc_state_t state = airy_svc_get_state(svc);

    switch (state) {
    case AIRY_SVC_STATE_READY:
    case AIRY_SVC_STATE_RUNNING:
    case AIRY_SVC_STATE_PAUSED:
        return AIRY_SUCCESS;

    case AIRY_SVC_STATE_ERROR:
        return DAEMON_EHEALTH;

    default:
        return DAEMON_ESTATE;
    }
}

/* 服务能力查询 */

bool airy_svc_has_capability(airy_svc_t svc, airy_svc_capability_t capability)
{

    if (!svc) {
        return false;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return (service->capabilities & capability) != 0;
}

/* 服务状态字符串转换 */

const char *airy_svc_state_to_string(airy_svc_state_t state)
{
    static const char *state_strings[] = {"NONE",   "CREATED",  "INITIALIZING", "READY",  "RUNNING",
                                          "PAUSED", "STOPPING", "STOPPED",      "ZOMBIE", "ERROR"};

    if (state < AIRY_SVC_STATE_NONE || state > AIRY_SVC_STATE_ERROR) {
        return "UNKNOWN";
    }

    return state_strings[state];
}

airy_svc_state_t airy_svc_state_from_string(const char *str)
{
    if (!str) {
        return AIRY_SVC_STATE_NONE;
    }

    static const struct {
        const char *name;
        airy_svc_state_t state;
    } state_map[] = {{"NONE", AIRY_SVC_STATE_NONE},
                     {"CREATED", AIRY_SVC_STATE_CREATED},
                     {"INITIALIZING", AIRY_SVC_STATE_INITIALIZING},
                     {"READY", AIRY_SVC_STATE_READY},
                     {"RUNNING", AIRY_SVC_STATE_RUNNING},
                     {"PAUSED", AIRY_SVC_STATE_PAUSED},
                     {"STOPPING", AIRY_SVC_STATE_STOPPING},
                     {"STOPPED", AIRY_SVC_STATE_STOPPED},
                     {"ZOMBIE", AIRY_SVC_STATE_ZOMBIE},
                     {"ERROR", AIRY_SVC_STATE_ERROR},
                     {NULL, AIRY_SVC_STATE_NONE}};

    for (int i = 0; state_map[i].name; i++) {
        if (strcasecmp(str, state_map[i].name) == 0) {
            return state_map[i].state;
        }
    }

    return AIRY_SVC_STATE_NONE;
}

/* 服务注册表 */

airy_err_t airy_svc_register(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    if (!g_registry.initialized) {
        airy_err_t err = svc_common_module_init();
        if (err != AIRY_SUCCESS) {
            return err;
        }
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;
    airy_mtx_lock(&g_registry.registry_mutex);

    for (airy_svc_internal_t *current = g_registry.services; current;
         current = current->next) {
        if (current == internal) {
            airy_mtx_unlock(&g_registry.registry_mutex);
            LOG_DEBUG("Service '%s' already registered", internal->name);
            return AIRY_SUCCESS;
        }
    }

    internal->next = g_registry.services;
    g_registry.services = internal;
    g_registry.service_count++;

    airy_mtx_unlock(&g_registry.registry_mutex);

    LOG_INFO("Service '%s' explicitly registered (total: %u)", internal->name,
             g_registry.service_count);
    return AIRY_SUCCESS;
}

airy_err_t airy_svc_unregister(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    if (!g_registry.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;
    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t **prev = &g_registry.services;
    airy_svc_internal_t *current = g_registry.services;

    while (current) {
        if (current == internal) {
            *prev = current->next;
            g_registry.service_count--;

            airy_mtx_unlock(&g_registry.registry_mutex);
            LOG_INFO("Service '%s' unregistered (remaining: %u)", internal->name,
                     g_registry.service_count);
            return AIRY_SUCCESS;
        }

        prev = &current->next;
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);
    LOG_WARN("Service '%s' not found in registry for unregistration", internal->name);
    return AIRY_ENOENT;
}

airy_svc_t airy_svc_find(const char *name)
{
    if (!name || !g_registry.initialized) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t *service = find_service_internal(name);

    airy_mtx_unlock(&g_registry.registry_mutex);

    return (airy_svc_t)service;
}

uint32_t airy_svc_count(void)
{
    if (!g_registry.initialized) {
        return 0;
    }

    airy_mtx_lock(&g_registry.registry_mutex);
    uint32_t count = g_registry.service_count;
    airy_mtx_unlock(&g_registry.registry_mutex);

    return count;
}

void airy_svc_foreach(airy_svc_enum_fn callback, void *user_data)
{
    if (!callback || !g_registry.initialized) {
        return;
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t *current = g_registry.services;
    while (current) {
        callback((airy_svc_t)current, user_data);
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);
}

airy_err_t airy_svc_set_user_data(airy_svc_t service, void *user_data)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)service;
    airy_mtx_lock(&internal->state_mutex);
    internal->user_data = user_data;
    airy_mtx_unlock(&internal->state_mutex);

    return AIRY_SUCCESS;
}

void *airy_svc_get_user_data(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)service;
    airy_mtx_lock(&internal->state_mutex);
    void *data = internal->user_data;
    airy_mtx_unlock(&internal->state_mutex);

    return data;
}

/* ==================== 模块清理 ==================== */

/* 前向声明：g_monitor 定义在文件末尾 */
#define MAX_MONITORED_SERVICES 32
typedef struct {
    airy_svc_t service;
    airy_monitor_config_t config;
    airy_degradation_handler_t degradation_handler;
    void *degradation_user_data;
    bool active;
    uint32_t consecutive_failures;
    uint32_t restart_attempts;
    uint64_t last_check_time;
    uint64_t next_restart_time;
    bool degraded;
    airy_thread_t monitor_thread;
    atomic_int stop_requested;
} monitored_service_t;

static struct {
    monitored_service_t services[MAX_MONITORED_SERVICES];
    uint32_t count;
    bool initialized;
    airy_mtx_t mutex;
} g_monitor;

static void monitor_shutdown(void)
{
    if (g_monitor.initialized) {
        airy_mtx_destroy(&g_monitor.mutex);
        g_monitor.initialized = false;
        SVC_LOG_INFO("Monitor: mutex destroyed");
    }
}

void airy_svc_common_cleanup(void)
{
    airy_mem_stats_reporter_shutdown();
    monitor_shutdown();
    svc_common_module_cleanup();
}

/* ==================== 跨进程服务注册中心（Phase 3.2） ==================== */

#define MAX_REGISTRY_ENTRIES 64
#define HEARTBEAT_INTERVAL_MS 30000

typedef struct {
    airy_svc_metadata_t metadata;
    airy_svc_t service;
    bool registered;
    uint64_t register_time;
} registry_entry_t;

static struct {
    char registry_url[512];
    bool initialized;
    registry_entry_t entries[MAX_REGISTRY_ENTRIES];
    uint32_t entry_count;
    airy_mtx_t mutex;
} g_cross_registry = {0};

airy_err_t airy_cross_registry_init(const char *registry_url)
{
    if (!registry_url) {
        return AIRY_EINVAL;
    }

    airy_err_t err = AIRY_SUCCESS;

    err = airy_mtx_init(&g_cross_registry.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    if (g_cross_registry.initialized) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        return AIRY_SUCCESS;
    }

    if (safe_strcpy(g_cross_registry.registry_url, registry_url,
                    sizeof(g_cross_registry.registry_url)) != 0) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        return AIRY_EINVAL;
    }
    __builtin_memset(g_cross_registry.entries, 0, sizeof(g_cross_registry.entries));
    g_cross_registry.entry_count = 0;
    g_cross_registry.initialized = true;

    airy_mtx_unlock(&g_cross_registry.mutex);

    LOG_INFO("Service registry client initialized: %s", registry_url);
    return AIRY_SUCCESS;
}

airy_err_t airy_registry_register(airy_svc_t service,
                                          const airy_svc_metadata_t *metadata)
{
    if (!service || !metadata) {
        return AIRY_EINVAL;
    }

    if (!g_cross_registry.initialized) {
        LOG_WARN("Registry not initialized, using local-only registration");
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    if (g_cross_registry.entry_count >= MAX_REGISTRY_ENTRIES) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        LOG_ERROR("Registry full, cannot register service '%s'", metadata->name);
        return AIRY_ENOMEM;
    }

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            __builtin_memcpy(&g_cross_registry.entries[i].metadata, metadata,
                   sizeof(airy_svc_metadata_t));
            g_cross_registry.entries[i].metadata.last_heartbeat = airy_time_ms();
            airy_mtx_unlock(&g_cross_registry.mutex);
            LOG_INFO("Service '%s' re-registered in cross-process registry", metadata->name);
            return AIRY_SUCCESS;
        }
    }

    registry_entry_t *entry = &g_cross_registry.entries[g_cross_registry.entry_count];
    __builtin_memcpy(&entry->metadata, metadata, sizeof(airy_svc_metadata_t));
    entry->service = service;
    entry->registered = true;
    entry->register_time = airy_time_ms();
    entry->metadata.last_heartbeat = entry->register_time;
    g_cross_registry.entry_count++;

    airy_mtx_unlock(&g_cross_registry.mutex);

    LOG_INFO("Service '%s' registered in cross-process registry (type=%s, endpoint=%s)",
             metadata->name, metadata->service_type, metadata->endpoint);
    return AIRY_SUCCESS;
}

airy_err_t airy_registry_deregister(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            LOG_INFO("Service '%s' deregistered from cross-process registry",
                     g_cross_registry.entries[i].metadata.name);

            if (i < g_cross_registry.entry_count - 1) {
                g_cross_registry.entries[i] =
                    g_cross_registry.entries[g_cross_registry.entry_count - 1];
            }
            __builtin_memset(&g_cross_registry.entries[g_cross_registry.entry_count - 1], 0,
                   sizeof(registry_entry_t));
            g_cross_registry.entry_count--;

            airy_mtx_unlock(&g_cross_registry.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_cross_registry.mutex);
    return AIRY_ENOENT;
}

static bool tag_matches(const char *filter_tags, const char *service_tags)
{
    if (!filter_tags || !filter_tags[0])
        return true;
    if (!service_tags || !service_tags[0])
        return false;

    char filter_copy[AIRY_MAX_TAGS_LEN];
    if (safe_strcpy(filter_copy, filter_tags, sizeof(filter_copy)) != 0) {
        return false;
    }

    char *saveptr = NULL;
    char *token = strtok_r(filter_copy, ",", &saveptr);
    while (token) {
        while (*token == ' ')
            token++;
        if (strstr(service_tags, token)) {
            return true;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    return false;
}

airy_svc_metadata_t *airy_registry_discover(const char *service_type,
                                                      const char *filter_tags, size_t *result_count)
{
    if (!result_count) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    *result_count = 0;

    if (!g_cross_registry.initialized && g_cross_registry.entry_count == 0) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    size_t match_count = 0;
    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        registry_entry_t *entry = &g_cross_registry.entries[i];
        if (!entry->registered)
            continue;

        bool type_match = !service_type || !service_type[0] ||
                          strcmp(entry->metadata.service_type, service_type) == 0;
        bool tag_match = tag_matches(filter_tags, entry->metadata.tags);

        if (type_match && tag_match) {
            match_count++;
        }
    }

    if (match_count == 0) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    airy_svc_metadata_t *results = (airy_svc_metadata_t *)AIRY_CALLOC(
        match_count, sizeof(airy_svc_metadata_t));
    if (!results) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    size_t idx = 0;
    for (uint32_t i = 0; i < g_cross_registry.entry_count && idx < match_count; i++) {
        registry_entry_t *entry = &g_cross_registry.entries[i];
        if (!entry->registered)
            continue;

        bool type_match = !service_type || !service_type[0] ||
                          strcmp(entry->metadata.service_type, service_type) == 0;
        bool tag_match = tag_matches(filter_tags, entry->metadata.tags);

        if (type_match && tag_match) {
            __builtin_memcpy(&results[idx], &entry->metadata, sizeof(airy_svc_metadata_t));
            idx++;
        }
    }

    *result_count = match_count;
    airy_mtx_unlock(&g_cross_registry.mutex);

    LOG_DEBUG("Service discovery: found %zu services (type=%s, tags=%s)", match_count,
              service_type ? service_type : "*", filter_tags ? filter_tags : "*");
    return results;
}

void airy_registry_discover_free(airy_svc_metadata_t *results)
{
    if (results) {
        AIRY_FREE(results);
    }
}

airy_err_t airy_registry_heartbeat(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            g_cross_registry.entries[i].metadata.last_heartbeat = airy_time_ms();
            g_cross_registry.entries[i].metadata.state = airy_svc_get_state(service);
            g_cross_registry.entries[i].metadata.healthy =
                (airy_svc_healthcheck(service) == AIRY_SUCCESS);

            airy_svc_stats_t stats;
            if (airy_svc_get_stats(service, &stats) == AIRY_SUCCESS) {
                g_cross_registry.entries[i].metadata.current_load =
                    stats.current_concurrent > 0
                        ? (uint32_t)(stats.current_concurrent * 100 /
                                     (stats.peak_concurrent > 0 ? stats.peak_concurrent : 1))
                        : 0;
            }

            airy_mtx_unlock(&g_cross_registry.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_cross_registry.mutex);
    return AIRY_ENOENT;
}

void airy_registry_cleanup(void)
{
    airy_mtx_lock(&g_cross_registry.mutex);

    __builtin_memset(g_cross_registry.entries, 0, sizeof(g_cross_registry.entries));
    g_cross_registry.entry_count = 0;
    g_cross_registry.initialized = false;
    g_cross_registry.registry_url[0] = '\0';

    airy_mtx_unlock(&g_cross_registry.mutex);
    airy_mtx_destroy(&g_cross_registry.mutex);

    LOG_INFO("Service registry client cleaned up");
}

/* ==================== 配置管理（Phase 3.2） ==================== */

#define MAX_CONFIG_WATCHERS 32
#define MAX_CONFIG_PATH_LEN 512

typedef struct {
    char service_name[64];
    airy_config_change_callback_t callback;
    void *user_data;
    bool active;
} config_watcher_t;

static struct {
    config_watcher_t watchers[MAX_CONFIG_WATCHERS];
    uint32_t watcher_count;
    char config_base_path[MAX_CONFIG_PATH_LEN];
    bool initialized;
    airy_mtx_t mutex;
} g_config_mgr = {0};

static airy_err_t config_mgr_init(void)
{
    if (g_config_mgr.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = airy_mtx_init(&g_config_mgr.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    __builtin_memset(g_config_mgr.watchers, 0, sizeof(g_config_mgr.watchers));
    g_config_mgr.watcher_count = 0;
    if (safe_strcpy(g_config_mgr.config_base_path, "./config",
                    sizeof(g_config_mgr.config_base_path)) != 0) {
        airy_mtx_destroy(&g_config_mgr.mutex);
        return AIRY_EINVAL;
    }
    g_config_mgr.initialized = true;

    return AIRY_SUCCESS;
}

static void compute_simple_checksum(const char *data, size_t len, char *out, size_t out_size)
{
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)data[i];
    }
    snprintf(out, out_size, "%016llx", (unsigned long long)hash);
}

airy_err_t airy_config_load(const char *service_name, airy_config_t **config)
{
    if (!service_name || !config) {
        return AIRY_EINVAL;
    }

    config_mgr_init();

    char config_path[MAX_CONFIG_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(config_path, sizeof(config_path), "%s/%s.json", g_config_mgr.config_base_path,
             service_name);

    FILE *fp = fopen(config_path, "rb");
    if (!fp) {
        snprintf(config_path, sizeof(config_path), "%s/%s.yaml", g_config_mgr.config_base_path,
                 service_name);
        fp = fopen(config_path, "rb");
    }
    if (!fp) {
        snprintf(config_path, sizeof(config_path), "%s/%s.toml", g_config_mgr.config_base_path,
                 service_name);
        fp = fopen(config_path, "rb");
    }
#pragma GCC diagnostic pop

    airy_config_t *cfg = (airy_config_t *)AIRY_CALLOC(1, sizeof(airy_config_t));
    if (!cfg) {
        return AIRY_ENOMEM;
    }

    if (!fp) {
        cfg->raw_config = AIRY_CALLOC(1, 1);
        cfg->config_size = 0;
        cfg->version = 1;
        cfg->last_modified = time(NULL);
        cfg->checksum[0] = '\0';
        *config = cfg;
        LOG_WARN("No config file found for service '%s', using empty config", service_name);
        return AIRY_SUCCESS;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fp);
        cfg->raw_config = AIRY_CALLOC(1, 1);
        cfg->config_size = 0;
        cfg->version = 1;
        cfg->last_modified = time(NULL);
        *config = cfg;
        return AIRY_SUCCESS;
    }

    cfg->raw_config = (char *)AIRY_CALLOC(1, file_size + 1);
    if (!cfg->raw_config) {
        fclose(fp);
        AIRY_FREE(cfg);
        return AIRY_ENOMEM;
    }

    size_t bytes_read = fread(cfg->raw_config, 1, file_size, fp);
    fclose(fp);
    if (bytes_read != (size_t)file_size) {
        AIRY_FREE(cfg->raw_config);
        AIRY_FREE(cfg);
        return AIRY_EIO;
    }

    cfg->config_size = bytes_read;
    cfg->raw_config[bytes_read] = '\0';
    cfg->version = 1;
    cfg->last_modified = time(NULL);
    compute_simple_checksum(cfg->raw_config, bytes_read, cfg->checksum, sizeof(cfg->checksum));

    *config = cfg;

    LOG_INFO("Config loaded for service '%s': %zu bytes from %s", service_name, bytes_read,
             config_path);
    return AIRY_SUCCESS;
}

airy_err_t airy_config_watch(const char *service_name,
                                     airy_config_change_callback_t callback, void *user_data)
{
    if (!service_name || !callback) {
        return AIRY_EINVAL;
    }

    config_mgr_init();

    airy_mtx_lock(&g_config_mgr.mutex);

    if (g_config_mgr.watcher_count >= MAX_CONFIG_WATCHERS) {
        airy_mtx_unlock(&g_config_mgr.mutex);
        return AIRY_ENOMEM;
    }

    for (uint32_t i = 0; i < g_config_mgr.watcher_count; i++) {
        if (g_config_mgr.watchers[i].active &&
            strcmp(g_config_mgr.watchers[i].service_name, service_name) == 0 &&
            g_config_mgr.watchers[i].callback == callback) {
            g_config_mgr.watchers[i].user_data = user_data;
            airy_mtx_unlock(&g_config_mgr.mutex);
            return AIRY_SUCCESS;
        }
    }

    config_watcher_t *watcher = &g_config_mgr.watchers[g_config_mgr.watcher_count];
    if (safe_strcpy(watcher->service_name, service_name, sizeof(watcher->service_name)) != 0) {
        airy_mtx_unlock(&g_config_mgr.mutex);
        return AIRY_EINVAL;
    }
    watcher->callback = callback;
    watcher->user_data = user_data;
    watcher->active = true;
    g_config_mgr.watcher_count++;

    airy_mtx_unlock(&g_config_mgr.mutex);

    LOG_INFO("Config watcher registered for service '%s'", service_name);
    return AIRY_SUCCESS;
}

airy_err_t airy_config_unwatch(const char *service_name,
                                       airy_config_change_callback_t callback)
{
    if (!service_name) {
        return AIRY_EINVAL;
    }

    if (!g_config_mgr.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_mtx_lock(&g_config_mgr.mutex);

    for (uint32_t i = 0; i < g_config_mgr.watcher_count; i++) {
        if (g_config_mgr.watchers[i].active &&
            strcmp(g_config_mgr.watchers[i].service_name, service_name) == 0) {
            if (callback == NULL || g_config_mgr.watchers[i].callback == callback) {
                g_config_mgr.watchers[i].active = false;

                if (i < g_config_mgr.watcher_count - 1) {
                    g_config_mgr.watchers[i] =
                        g_config_mgr.watchers[g_config_mgr.watcher_count - 1];
                }
                g_config_mgr.watcher_count--;
                if (callback == NULL) {
                    continue;
                }
            }
        }
    }

    airy_mtx_unlock(&g_config_mgr.mutex);
    return AIRY_SUCCESS;
}

void airy_config_free(airy_config_t *config)
{
    if (!config) {
        return;
    }

    if (config->raw_config) {
        AIRY_FREE(config->raw_config);
        config->raw_config = NULL;
    }

    AIRY_FREE(config);
}

/* ==================== 故障恢复（Phase 3.3） ==================== */

static airy_err_t monitor_init(void)
{
    if (g_monitor.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = airy_mtx_init(&g_monitor.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    __builtin_memset(g_monitor.services, 0, sizeof(g_monitor.services));
    g_monitor.count = 0;
    g_monitor.initialized = true;

    return AIRY_SUCCESS;
}

static void *monitor_thread_func(void *arg)
{
    monitored_service_t *mon = (monitored_service_t *)arg;
    if (!mon || !mon->service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    const char *svc_name = airy_svc_get_name(mon->service);
    uint32_t interval_ms = mon->config.healthcheck_interval_ms;
    if (interval_ms == 0)
        interval_ms = 30000;

    LOG_INFO("Monitor thread started for service '%s' (interval=%ums)", svc_name, interval_ms);

    while (!mon->stop_requested && mon->active) {
        airy_sleep_ms(interval_ms);

        if (mon->stop_requested || !mon->active)
            break;

        airy_err_t err = airy_svc_healthcheck(mon->service);
        mon->last_check_time = airy_time_ms();

        if (err != AIRY_SUCCESS) {
            mon->consecutive_failures++;
            LOG_WARN("Service '%s' health check failed (consecutive: %u)", svc_name,
                     mon->consecutive_failures);

            if (mon->config.enable_degradation &&
                mon->consecutive_failures >= mon->config.degradation_threshold && !mon->degraded &&
                mon->degradation_handler) {
                mon->degraded = true;
                char reason[128];
                snprintf(reason, sizeof(reason), "consecutive_failures=%u >= threshold=%u",
                         mon->consecutive_failures, mon->config.degradation_threshold);
                mon->degradation_handler(mon->service, reason, mon->degradation_user_data);
                LOG_WARN("Service '%s' degraded: %s", svc_name, reason);
            }

            if (mon->config.auto_restart &&
                mon->restart_attempts < mon->config.max_restart_attempts) {
                uint64_t now = airy_time_ms();
                if (now >= mon->next_restart_time) {
                    mon->restart_attempts++;
                    LOG_INFO("Auto-restarting service '%s' (attempt %u/%u)", svc_name,
                             mon->restart_attempts, mon->config.max_restart_attempts);
                    airy_svc_stop(mon->service, true);
                    airy_err_t start_err = airy_svc_start(mon->service);
                    if (start_err == AIRY_SUCCESS) {
                        mon->consecutive_failures = 0;
                        mon->degraded = false;
                        LOG_INFO("Service '%s' restarted successfully", svc_name);
                    } else {
                        uint32_t backoff = mon->config.restart_backoff_base_ms *
                                           (1 << (mon->restart_attempts - 1));
                        if (backoff > mon->config.restart_backoff_max_ms)
                            backoff = mon->config.restart_backoff_max_ms;
                        mon->next_restart_time = now + backoff;
                        LOG_ERROR("Service '%s' restart failed, next retry in %ums", svc_name,
                                  backoff);
                    }
                }
            }
        } else {
            if (mon->consecutive_failures > 0)
                LOG_INFO("Service '%s' recovered after %u failures", svc_name,
                         mon->consecutive_failures);
            mon->consecutive_failures = 0;
            mon->restart_attempts = 0;
            mon->degraded = false;
        }
    }

    LOG_INFO("Monitor thread stopped for service '%s'", svc_name);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

airy_err_t airy_svc_monitor_start(airy_svc_t service,
                                              const airy_monitor_config_t *config)
{
    if (!service || !config) {
        return AIRY_EINVAL;
    }

    monitor_init();

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].stop_requested = 1;
            if (g_monitor.services[i].monitor_thread) {
                airy_mtx_unlock(&g_monitor.mutex);
                airy_thread_join(g_monitor.services[i].monitor_thread, NULL);
                airy_mtx_lock(&g_monitor.mutex);
            }
            __builtin_memcpy(&g_monitor.services[i].config, config, sizeof(airy_monitor_config_t));
            g_monitor.services[i].active = true;
            g_monitor.services[i].consecutive_failures = 0;
            g_monitor.services[i].restart_attempts = 0;
            g_monitor.services[i].degraded = false;
            g_monitor.services[i].stop_requested = 0;
            g_monitor.services[i].last_check_time = airy_time_ms();
            g_monitor.services[i].next_restart_time = 0;

            int thread_err = airy_thread_create(&g_monitor.services[i].monitor_thread,
                                                   monitor_thread_func, &g_monitor.services[i]);
            if (thread_err != 0) {
                g_monitor.services[i].active = false;
                airy_mtx_unlock(&g_monitor.mutex);
                LOG_ERROR("Failed to create monitor thread for service '%s'",
                          airy_svc_get_name(service));
                return DAEMON_EINIT;
            }

            airy_mtx_unlock(&g_monitor.mutex);
            LOG_INFO("Service monitoring updated for '%s'", airy_svc_get_name(service));
            return AIRY_SUCCESS;
        }
    }

    if (g_monitor.count >= MAX_MONITORED_SERVICES) {
        airy_mtx_unlock(&g_monitor.mutex);
        return AIRY_ENOMEM;
    }

    monitored_service_t *mon = &g_monitor.services[g_monitor.count];
    mon->service = service;
    __builtin_memcpy(&mon->config, config, sizeof(airy_monitor_config_t));
    mon->degradation_handler = NULL;
    mon->degradation_user_data = NULL;
    mon->active = true;
    mon->consecutive_failures = 0;
    mon->restart_attempts = 0;
    mon->last_check_time = airy_time_ms();
    mon->next_restart_time = 0;
    mon->degraded = false;
    mon->stop_requested = 0;
    mon->monitor_thread = (airy_thread_t)0;

    int thread_err = airy_thread_create(&mon->monitor_thread, monitor_thread_func, mon);
    if (thread_err != 0) {
        mon->active = false;
        airy_mtx_unlock(&g_monitor.mutex);
        LOG_ERROR("Failed to create monitor thread for service '%s'",
                  airy_svc_get_name(service));
        return DAEMON_EINIT;
    }

    g_monitor.count++;

    airy_mtx_unlock(&g_monitor.mutex);

    LOG_INFO("Service monitoring started for '%s' (interval=%ums, auto_restart=%s)",
             airy_svc_get_name(service), config->healthcheck_interval_ms,
             config->auto_restart ? "true" : "false");
    return AIRY_SUCCESS;
}

airy_err_t airy_svc_monitor_stop(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    if (!g_monitor.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].stop_requested = 1;
            g_monitor.services[i].active = false;

            airy_thread_t thread = g_monitor.services[i].monitor_thread;

            airy_mtx_unlock(&g_monitor.mutex);

            if (thread) {
                airy_thread_join(thread, NULL);
            }

            airy_mtx_lock(&g_monitor.mutex);

            for (uint32_t j = 0; j < g_monitor.count; j++) {
                if (g_monitor.services[j].service == service) {
                    LOG_INFO("Service monitoring stopped for '%s'",
                             airy_svc_get_name(service));
                    if (j < g_monitor.count - 1) {
                        g_monitor.services[j] = g_monitor.services[g_monitor.count - 1];
                    }
                    __builtin_memset(&g_monitor.services[g_monitor.count - 1], 0,
                           sizeof(monitored_service_t));
                    g_monitor.count--;
                    break;
                }
            }

            airy_mtx_unlock(&g_monitor.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_monitor.mutex);
    return AIRY_ENOENT;
}

airy_err_t airy_svc_set_degradation_handler(airy_svc_t service,
                                                        airy_degradation_handler_t handler,
                                                        void *user_data)
{
    if (!service || !handler) {
        return AIRY_EINVAL;
    }

    monitor_init();

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].degradation_handler = handler;
            g_monitor.services[i].degradation_user_data = user_data;
            airy_mtx_unlock(&g_monitor.mutex);
            LOG_INFO("Degradation handler set for service '%s'", airy_svc_get_name(service));
            return AIRY_SUCCESS;
        }
    }

    if (g_monitor.count < MAX_MONITORED_SERVICES) {
        monitored_service_t *mon = &g_monitor.services[g_monitor.count];
        mon->service = service;
        mon->degradation_handler = handler;
        mon->degradation_user_data = user_data;
        mon->active = false;
        mon->consecutive_failures = 0;
        mon->restart_attempts = 0;
        mon->degraded = false;
        g_monitor.count++;

        airy_mtx_unlock(&g_monitor.mutex);
        LOG_INFO("Degradation handler set for unmonitored service '%s'",
                 airy_svc_get_name(service));
        return AIRY_SUCCESS;
    }

    airy_mtx_unlock(&g_monitor.mutex);
    return AIRY_ENOMEM;
}

/* ==================== 服务间通信客户端（Phase 3.2） ==================== */

typedef struct {
    airy_svc_protocol_type_t protocol;
    char base_url[512];
    uint32_t default_timeout_ms;
} client_internal_t;

#ifdef AIRY_HAS_CURL
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} curl_response_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_response_buf_t *buf = (curl_response_buf_t *)userdata;
    size_t total = size * nmemb;
    if (buf->size + total + 1 > buf->capacity) {
        size_t new_cap = buf->capacity == 0 ? 4096 : buf->capacity;
        while (new_cap < buf->size + total + 1)
            new_cap *= 2;
        char *new_data = (char *)AIRY_REALLOC(buf->data, new_cap);
        if (!new_data)
            return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    /* SEC-02: 二次校验，防御 realloc 异常返回 */
    if (buf->size + total > buf->capacity) {
        airy_err_push_ex(AIRY_EOVERFLOW, __FILE__, __LINE__, __func__,
                               "curl_write_cb: buffer overflow");
        return 0;
    }
    __builtin_memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}
#endif

static airy_err_t http_client_call(const char *service_name, const char *method,
                                        const char *params_json, char **response_json,
                                        uint32_t timeout_ms)
{
    if (!service_name || !method || !response_json) {
        return AIRY_EINVAL;
    }

    LOG_DEBUG("HTTP client call: %s/%s (timeout=%ums)", service_name, method, timeout_ms);

    *response_json = NULL;

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            airy_err_t err = internal->iface.handle_request(
                svc, method, params_json, response_json, internal->user_data);
            if (err != AIRY_SUCCESS) {
                LOG_WARN("Service '%s' handle_request('%s') failed: %d", service_name, method, err);
                return err;
            }
            if (!*response_json) {
                *response_json = AIRY_CALLOC(1, 2);
                if (*response_json) {
                    (*response_json)[0] = '{';
                    (*response_json)[1] = '}';
                }
            }
            return AIRY_SUCCESS;
        }

        airy_svc_state_t state = airy_svc_get_state(svc);
        if (state != AIRY_SVC_STATE_RUNNING) {
            LOG_WARN("Service '%s' not running (state=%s), cannot handle request", service_name,
                     airy_svc_state_to_string(state));
            return DAEMON_ESTATE;
        }

        LOG_WARN("Service '%s' has no handle_request callback", service_name);
        return AIRY_ESERVICE;
    }

    char url[768];
    snprintf(url, sizeof(url), "http://%s/api/%s", service_name, method);

#ifdef AIRY_HAS_CURL
    airy_err_t ret_err = AIRY_SUCCESS;
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize CURL for remote call to '%s'", service_name);
        return DAEMON_EINIT;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);

    if (params_json) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params_json);
    }

    curl_response_buf_t resp_buf = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("CURL call to '%s' failed: %s", url, curl_easy_strerror(res));
        ret_err = AIRY_EIO;
        goto cleanup;
    }

    if (http_code >= 400) {
        LOG_ERROR("Service '%s' returned HTTP %ld", service_name, http_code);
        ret_err = AIRY_EIO;
        goto cleanup;
    }

    if (!resp_buf.data || resp_buf.size == 0) {
        *response_json = AIRY_CALLOC(1, 2);
        if (*response_json) {
            (*response_json)[0] = '{';
            (*response_json)[1] = '}';
        }
    } else {
        *response_json = resp_buf.data;
    }

    return AIRY_SUCCESS;

cleanup:
    AIRY_FREE(resp_buf.data);
    return ret_err;
#else
    LOG_ERROR("Remote call to '%s' failed: libcurl not available", service_name);
    return AIRY_EIO;
#endif
}

static airy_err_t http_client_stream(const char *service_name, const char *method,
                                          const char *params_json,
                                          airy_stream_callback_t callback, void *user_data)
{
    if (!service_name || !method || !callback) {
        return AIRY_EINVAL;
    }

    LOG_DEBUG("HTTP client stream: %s/%s", service_name, method);

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            char *response = NULL;
            airy_err_t err = internal->iface.handle_request(svc, method, params_json,
                                                                 &response, internal->user_data);
            if (err == AIRY_SUCCESS && response) {
                callback(response, strlen(response), user_data);
                AIRY_FREE(response);
            } else {
                const char *err_json = "{\"error\":\"stream_failed\"}";
                callback(err_json, strlen(err_json), user_data);
            }
            return err;
        }

        const char *err_json = "{\"error\":\"no_stream_handler\"}";
        callback(err_json, strlen(err_json), user_data);
        return AIRY_ESERVICE;
    }

    return AIRY_ENOENT;
}

static airy_err_t memory_client_call(const char *service_name, const char *method,
                                          const char *params_json, char **response_json,
                                          uint32_t timeout_ms)
{
    if (!service_name || !method || !response_json) {
        return AIRY_EINVAL;
    }

    airy_svc_t svc = airy_svc_find(service_name);
    if (svc) {
        airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

        if (internal->iface.handle_request) {
            airy_err_t err = internal->iface.handle_request(
                svc, method, params_json, response_json, internal->user_data);
            if (err != AIRY_SUCCESS) {
                LOG_WARN("Service '%s' handle_request('%s') failed: %d", service_name, method, err);
                return err;
            }
            if (!*response_json) {
                *response_json = (char *)AIRY_CALLOC(1, 2);
                if (*response_json) {
                    (*response_json)[0] = '{';
                    (*response_json)[1] = '}';
                }
            }
            return AIRY_SUCCESS;
        }

        LOG_WARN("Service '%s' has no handle_request callback", service_name);
        return AIRY_ESERVICE;
    }

    LOG_INFO("Memory client: service '%s' not found locally, trying IPC RPC", service_name);

    char rpc_method[256];
    snprintf(rpc_method, sizeof(rpc_method), "%s.%s", service_name, method);

    int rpc_err =
        svc_rpc_call(rpc_method, params_json, response_json, timeout_ms ? timeout_ms : 30000);
    if (rpc_err != 0 || !(*response_json)) {
        LOG_WARN("IPC RPC call to '%s' failed: %d", rpc_method, rpc_err);
        return AIRY_EIO;
    }

    LOG_INFO("IPC RPC call to '%s' succeeded", rpc_method);
    return AIRY_SUCCESS;
}

static airy_err_t memory_client_stream(const char *service_name, const char *method,
                                            const char *params_json,
                                            airy_stream_callback_t callback, void *user_data)
{
    if (!service_name || !method || !callback) {
        return AIRY_EINVAL;
    }

    airy_svc_t svc = airy_svc_find(service_name);
    if (!svc) {
        return AIRY_ENOENT;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;

    if (internal->iface.handle_request) {
        char *response = NULL;
        airy_err_t err = internal->iface.handle_request(svc, method, params_json, &response,
                                                             internal->user_data);
        if (err == AIRY_SUCCESS && response) {
            callback(response, strlen(response), user_data);
            AIRY_FREE(response);
        } else {
            const char *err_json = "{\"error\":\"stream_failed\"}";
            callback(err_json, strlen(err_json), user_data);
        }
        return err;
    }

    const char *err_json = "{\"error\":\"no_stream_handler\"}";
    callback(err_json, strlen(err_json), user_data);
    return AIRY_ESERVICE;
}

airy_err_t airy_svc_client_create(airy_svc_protocol_type_t protocol,
                                              const char *config, airy_svc_client_t **client)
{
    if (!client) {
        return AIRY_EINVAL;
    }

    client_internal_t *internal = (client_internal_t *)AIRY_CALLOC(1, sizeof(client_internal_t));
    if (!internal) {
        return AIRY_ENOMEM;
    }

    internal->protocol = protocol;
    internal->default_timeout_ms = 30000;

    if (config) {
        if (safe_strcpy(internal->base_url, config, sizeof(internal->base_url)) != 0) {
            AIRY_FREE(internal);
            return AIRY_EINVAL;
        }
    } else {
        if (safe_strcpy(internal->base_url, "http://localhost:8080", sizeof(internal->base_url)) !=
            0) {
            AIRY_FREE(internal);
            return AIRY_EINVAL;
        }
    }

    airy_svc_client_t *cli =
        (airy_svc_client_t *)AIRY_CALLOC(1, sizeof(airy_svc_client_t));
    if (!cli) {
        AIRY_FREE(internal);
        return AIRY_ENOMEM;
    }

    switch (protocol) {
    case SVC_PROTO_HTTP:
        cli->call = http_client_call;
        cli->stream = http_client_stream;
        break;
    case SVC_PROTO_MEMORY:
        cli->call = memory_client_call;
        cli->stream = memory_client_stream;
        break;
    default:
        cli->call = http_client_call;
        cli->stream = http_client_stream;
        LOG_WARN("Protocol %d not fully implemented, using HTTP fallback", protocol);
        break;
    }

    *client = cli;
    cli->internal = internal;

    LOG_INFO("Service client created (protocol=%d, base_url=%s)", protocol, internal->base_url);
    return AIRY_SUCCESS;
}

void airy_svc_client_destroy(airy_svc_client_t *client)
{
    if (!client) {
        return;
    }
    if (client->internal) {
        AIRY_FREE(client->internal);
        client->internal = NULL;
    }
    AIRY_FREE(client);
    LOG_DEBUG("Service client destroyed");
}