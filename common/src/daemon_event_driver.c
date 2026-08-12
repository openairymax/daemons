// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "daemon_event_driver.h"

#include "jsonrpc_helpers.h"
#include "airy_memory.h"
#include "method_dispatcher.h"
#include "svc_logger.h"
/* P0.17 phase 2: daemon_event_driver.c uses airy_sock_* daemon-specific
 * functions, so include daemon_platform_ext.h for their declarations (the
 * commons platform.h lacks them). */
#include "daemon_platform_ext.h"

#include <stdlib.h>
#include <string.h>
#include "error.h"

struct daemon_event_driver {
    airy_event_loop_t *loop;
    thread_pool_t *pool;
    method_dispatcher_t *dispatcher;
    daemon_on_client_cb on_client;
    daemon_on_timer_cb on_timer;
    void *service_ctx;
    bool use_jsonrpc;
    bool concurrent_clients;
    int health_check_interval_sec;
    uint64_t health_timer_id;
};

static void socket_close_wrapper(void *arg)
{
    airy_sock_close((airy_sock_t)(uintptr_t)arg);
}

/* Concurrent client-processing task: wraps the on_client callback +
 * service_ctx + client_fd, executed by a thread-pool worker so the
 * event-loop thread is not blocked (needed for interactive approval). */
typedef struct on_client_task {
    daemon_on_client_cb cb;
    void *service_ctx;
    airy_sock_t client_fd;
} on_client_task_t;

static void on_client_task_runner(void *arg)
{
    on_client_task_t *task = (on_client_task_t *)arg;
    if (!task) {
        return;
    }
    daemon_on_client_cb cb = task->cb;
    void *ctx = task->service_ctx;
    airy_sock_t fd = task->client_fd;
    AIRY_FREE(task);
    if (cb) {
        cb(ctx, fd);
    }
}

static int on_server_fd_event(int fd, uint32_t events, void *user_data)
{
    daemon_event_driver_t *driver = (daemon_event_driver_t *)user_data;
    if (!driver)
        return AIRY_ERR_INVALID_PARAM;

    if (!(events & AIRY_EVENT_TYPE_READ))
        return 0;

    int first = 1;
    while (1) {
        airy_sock_t client_fd = airy_sock_accept(fd, 0);
        if (client_fd < 0) {
            /* <0 covers AIRY_INVALID_SOCKET(-1) and negative error codes
             * (e.g. AIRY_ERR_IO), preventing negative codes like -40 from
             * being misjudged as valid connections and logged as
             * CLIENT-ACCEPT */
            if (first) {
                SVC_LOG_ERROR("C-L02: EVENT-DRIVER: CLIENT-ERROR accept failed on fd=%d", fd);
            }
            break;
        }

        SVC_LOG_INFO("C-L02: EVENT-DRIVER: CLIENT-ACCEPT fd=%d", (int)(uintptr_t)client_fd);

        if (driver->concurrent_clients && driver->on_client && driver->pool) {
            /* Concurrent mode: dispatch the client request to the thread
             * pool; the event-loop thread returns immediately to keep
             * accepting/handling other connections. on_client_task_runner
             * executes on_client on a worker thread and closes the fd via
             * daemon_handle_client afterwards. */
            on_client_task_t *task = (on_client_task_t *)AIRY_CALLOC(1, sizeof(on_client_task_t));
            if (task) {
                task->cb = driver->on_client;
                task->service_ctx = driver->service_ctx;
                task->client_fd = client_fd;
                if (thread_pool_submit(driver->pool, on_client_task_runner, task) != 0) {
                    SVC_LOG_ERROR("C-L02: EVENT-DRIVER: CLIENT-ERROR pool submit failed fd=%d",
                                  (int)(uintptr_t)client_fd);
                    AIRY_FREE(task);
                    airy_sock_close(client_fd);
                }
            } else {
                SVC_LOG_ERROR("C-L02: EVENT-DRIVER: CLIENT-ERROR alloc task failed fd=%d",
                              (int)(uintptr_t)client_fd);
                airy_sock_close(client_fd);
            }
        } else if (driver->on_client) {
            driver->on_client(driver->service_ctx, client_fd);
        } else if (driver->pool) {
            thread_pool_submit(driver->pool, socket_close_wrapper, (void *)(uintptr_t)client_fd);
        } else {
            airy_sock_close(client_fd);
        }
        first = 0;
    }

    return 0;
}

static void on_health_timer(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    SVC_LOG_DEBUG("C-L02: EVENT-DRIVER: HEALTH-CHECK timer_id=%lu", (unsigned long)timer_id);
    daemon_event_driver_t *driver = (daemon_event_driver_t *)user_data;
    if (driver && driver->on_timer) {
        driver->on_timer(driver->service_ctx, timer_id);
    }
}

daemon_event_driver_t *daemon_event_driver_create(const daemon_event_config_t *config)
{
    if (!config) {
        SVC_LOG_ERROR(
            "C-L02: EVENT-DRIVER: CREATE-FAIL null config, STACK: daemon_event_driver_create");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    daemon_event_driver_t *driver =
        (daemon_event_driver_t *)AIRY_CALLOC(1, sizeof(daemon_event_driver_t));
    if (!driver) {
        SVC_LOG_ERROR(
            "C-L02: EVENT-DRIVER: CREATE-FAIL alloc driver, STACK: daemon_event_driver_create");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    int max_events = config->max_events > 0 ? config->max_events : 64;
    driver->loop = airy_event_loop_create(max_events);
    if (!driver->loop) {
        SVC_LOG_ERROR("C-L02: EVENT-DRIVER: CREATE-FAIL loop, STACK: daemon_event_driver_create");
        AIRY_FREE(driver);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (config->thread_pool_max > 0) {
        thread_pool_config_t tp_config;
        __builtin_memset(&tp_config, 0, sizeof(tp_config));
        tp_config.min_threads = config->thread_pool_min > 0 ? config->thread_pool_min : 2;
        tp_config.max_threads = config->thread_pool_max;
        tp_config.queue_size =
            config->thread_pool_queue_size > 0 ? config->thread_pool_queue_size : 256;
        tp_config.idle_timeout_ms = 30000;
        driver->pool = thread_pool_create(&tp_config);
        if (!driver->pool) {
            SVC_LOG_WARN("C-L02: EVENT-DRIVER: CREATE-WARN thread pool failed, STACK: "
                         "daemon_event_driver_create");
        }
    }

    if (config->use_jsonrpc) {
        /* 32-method capacity: cupolas_d registers 17 (incl. the vault/net/
         * entitlements wiring methods), all other daemons stay within 16.
         * This capacity is an upper-bound guard; registration overflow warns
         * and is rejected. */
        driver->dispatcher = method_dispatcher_create(32);
        if (!driver->dispatcher) {
            SVC_LOG_ERROR(
                "C-L02: EVENT-DRIVER: CREATE-FAIL dispatcher, STACK: daemon_event_driver_create");
            if (driver->pool)
                thread_pool_destroy(driver->pool);
            airy_event_loop_destroy(driver->loop);
            AIRY_FREE(driver);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }
    }

    driver->on_client = config->on_client;
    driver->on_timer = config->on_timer;
    driver->service_ctx = config->service_ctx;
    driver->use_jsonrpc = config->use_jsonrpc;
    driver->concurrent_clients = config->concurrent_clients;
    driver->health_check_interval_sec =
        config->health_check_interval_sec > 0 ? config->health_check_interval_sec : 30;

    if (config->on_timer) {
        uint64_t interval_ms = (uint64_t)driver->health_check_interval_sec * 1000;
        driver->health_timer_id =
            airy_event_loop_add_timer(driver->loop, interval_ms, on_health_timer, driver);
    }

    SVC_LOG_INFO(
        "C-L02: EVENT-DRIVER: CREATE-OK max_events=%d pool=%s jsonrpc=%s health_check_interval=%ds",
        max_events, driver->pool ? "on" : "off", driver->dispatcher ? "on" : "off",
        driver->health_check_interval_sec);

    return driver;
}

void daemon_event_driver_destroy(daemon_event_driver_t *driver)
{
    if (!driver)
        return;
    SVC_LOG_INFO("C-L02: EVENT-DRIVER: DESTROY");
    if (driver->pool)
        thread_pool_destroy(driver->pool);
    if (driver->dispatcher)
        method_dispatcher_destroy(driver->dispatcher);
    if (driver->loop)
        airy_event_loop_destroy(driver->loop);
    AIRY_FREE(driver);
}

int daemon_event_driver_add_server_fd(daemon_event_driver_t *driver, int fd)
{
    if (!driver || fd < 0)
        return AIRY_ERR_INVALID_PARAM;
    return airy_event_loop_add_fd_lt(driver->loop, fd, AIRY_EVENT_TYPE_READ, on_server_fd_event,
                                     driver);
}

int daemon_event_driver_add_fd(daemon_event_driver_t *driver, int fd, uint32_t events,
                               airy_event_callback_t cb, void *user_data)
{
    if (!driver || fd < 0 || !cb)
        return AIRY_ERR_INVALID_PARAM;
    return airy_event_loop_add_fd(driver->loop, fd, events, cb, user_data);
}

uint64_t daemon_event_driver_add_timer(daemon_event_driver_t *driver, uint64_t interval_ms,
                                       airy_timer_callback_t cb, void *user_data)
{
    if (!driver || !cb)
        return 0;
    return airy_event_loop_add_timer(driver->loop, interval_ms, cb, user_data);
}

int daemon_event_driver_cancel_timer(daemon_event_driver_t *driver, uint64_t timer_id)
{
    if (!driver)
        return AIRY_ERR_INVALID_PARAM;
    return airy_event_loop_cancel_timer(driver->loop, timer_id);
}

int daemon_event_driver_run(daemon_event_driver_t *driver)
{
    if (!driver)
        return AIRY_ERR_INVALID_PARAM;
    SVC_LOG_INFO("C-L02: EVENT-DRIVER: RUN");
    return airy_event_loop_run(driver->loop);
}

void daemon_event_driver_stop(daemon_event_driver_t *driver)
{
    if (!driver)
        return;
    /* Stop async-safely first (reachable from signal context), then log:
     * avoids the log lock being held on the signal path and deadlocking the
     * stop flow. */
    airy_event_loop_stop_async(driver->loop);
    SVC_LOG_INFO("C-L02: EVENT-DRIVER: STOP");
}

void daemon_event_driver_stop_async(daemon_event_driver_t *driver)
{
    if (!driver)
        return;
    airy_event_loop_stop_async(driver->loop);
}

airy_event_loop_t *daemon_event_driver_get_loop(daemon_event_driver_t *driver)
{
    return driver ? driver->loop : NULL;
}

thread_pool_t *daemon_event_driver_get_pool(daemon_event_driver_t *driver)
{
    return driver ? driver->pool : NULL;
}

method_dispatcher_t *daemon_event_driver_get_dispatcher(daemon_event_driver_t *driver)
{
    return driver ? driver->dispatcher : NULL;
}
