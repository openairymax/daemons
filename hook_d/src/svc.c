/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief hook_d 生命周期策略域（gen5 手写层：注册表引导 / 安全 ops 注入 /
 *        P1-5 会话存储）。
 * @details 本户为常量户：端点由 svc_endpoint 从生成头常量回填，
 *          cmdline use_tcp 只升不降，不读取 config 文件。hook 核心不再
 *          链接 cupolas，本守护进程自持 SafetyGuard 实现并经
 *          airy_safety_ops_t 注入，供 atoms 调用点在 PRE_TOOL/PRE_EXEC
 *          拦截链使用；注入点在 svc_prepare，撤除点在 svc_destroy。
 *          airy_hook_init 失败按原语义非阻断降级（health 报不健康，
 *          服务继续受理）。会话锁的 init/destroy 与表存储同域归位。
 */

#include "airy_memory.h"
#include "svc_hook_d.h"
#include "hook_d_internal.h"
#include "airy_hook.h"
#include "airy_safety_ops.h"
#include "hook_builtin_handlers.h"
#include "safety_guard.h"
#include "svc_logger.h"

#include <string.h>
#include <time.h>

int g_registry_initialized = 0;
uint64_t g_start_time = 0;

hook_session_entry_t g_hook_sessions[AIRY_HOOK_MAX_SESSIONS];
airy_mtx_t g_hook_sessions_lock;

hook_session_entry_t *hook_session_find(const char *session_id)
{
    for (size_t i = 0; i < AIRY_HOOK_MAX_SESSIONS; i++) {
        if (g_hook_sessions[i].active &&
            strcmp(g_hook_sessions[i].session_id, session_id) == 0)
            return &g_hook_sessions[i];
    }
    return NULL;
}

hook_session_entry_t *hook_session_upsert(const char *session_id)
{
    hook_session_entry_t *e = hook_session_find(session_id);
    if (e)
        return e;
    size_t slot = AIRY_HOOK_MAX_SESSIONS;
    size_t oldest = 0;
    for (size_t i = 0; i < AIRY_HOOK_MAX_SESSIONS; i++) {
        if (!g_hook_sessions[i].active) {
            slot = i;
            break;
        }
        if (g_hook_sessions[i].started_ns < g_hook_sessions[oldest].started_ns)
            oldest = i;
    }
    if (slot == AIRY_HOOK_MAX_SESSIONS)
        slot = oldest; /* 全满：覆盖最旧会话 */
    AIRY_MEMSET(&g_hook_sessions[slot], 0, sizeof(g_hook_sessions[slot]));
    AIRY_STRNCPY_TERM(g_hook_sessions[slot].session_id, session_id,
                      sizeof(g_hook_sessions[slot].session_id));
    g_hook_sessions[slot].active = true;
    g_hook_sessions[slot].started_ns = (uint64_t)time(NULL) * 1000000000ull;
    return &g_hook_sessions[slot];
}

static safety_guard_context_t *hook_safety_create(void)
{
    return safety_guard_create();
}

static safety_decision_t hook_safety_check_chain(safety_guard_context_t *ctx,
                                                 const safety_event_t *event,
                                                 safety_result_t **results, size_t *result_count)
{
    return safety_guard_check_chain(ctx, event, results, result_count);
}

static void hook_safety_destroy(safety_guard_context_t *ctx)
{
    safety_guard_destroy(ctx);
}

static const airy_safety_ops_t g_hook_safety_ops = {
    .create = hook_safety_create,
    .check_chain = hook_safety_check_chain,
    .destroy = hook_safety_destroy,
};

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp;
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = HOOK_D_TCP_PORT;
    ep->sock_unix = HOOK_D_SOCKET_UNIX;
    ep->sock_win = HOOK_D_SOCKET_WIN;
}

int svc_prepare(const char *config_path)
{
    (void)config_path;

    airy_mtx_init(&g_hook_sessions_lock);
    are_ops_set_safety(&g_hook_safety_ops);
    g_start_time = (uint64_t)time(NULL);
    SVC_LOG_INFO("hook_d: starting");

    if (airy_hook_init() == 0) {
        g_registry_initialized = 1;
        SVC_LOG_INFO("hook_d: hook system initialized (registry + timeout manager)");
        /* 内置生产 handler（audit/metrics/trace）统一注册，
         * status/list 由此返回真实加载的 hook 模块信息 */
        airy_hook_register_builtin_handlers();
    } else {
        SVC_LOG_ERROR("hook_d: hook system init failed");
    }
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_registry_initialized) {
        airy_hook_unregister_builtin_handlers();
        /* airy_hook_init 是注册表 + 超时管理器的唯一生命周期入口；
         * 旧代码直调 hook_registry_destroy 会跳过超时管理器并绕过
         * 共享引用计数，此处保持归位后的唯一释放路径 */
        airy_hook_shutdown();
        g_registry_initialized = 0;
    }
    are_ops_set_safety(NULL);
    airy_mtx_destroy(&g_hook_sessions_lock);
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
