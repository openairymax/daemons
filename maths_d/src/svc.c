/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief maths_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 服务单例 + 生命周期四钩子 + 18 个 RPC handler 薄壳。handler 把
 * method_fn 签名翻译为协议无关的 maths_d_rpc_call，响应三路收敛：
 * OK → success；ERR_METHOD → -32601；ERR_DOMAIN → -32000 + err。
 * 全部业务逻辑在域层 maths_service.c；Python 后端不可用时降级
 * 纯 C 路径，不阻塞 daemon 启动。
 */

#include "svc_maths_d.h"

#include "jsonrpc_helpers.h"
#include "maths_service.h"
#include "svc_logger.h"

#include <string.h>

/* 服务单例：机制层经钩子间接驱动，不直接访问 */
static maths_d_service_t g_svc_maths_d;

int svc_prepare(const char *config_path)
{
    (void)config_path;
    if (maths_d_service_init(&g_svc_maths_d) != 0) {
        SVC_LOG_ERROR("maths_d: service init failed");
        return -1;
    }
    if (maths_backend_init(&g_svc_maths_d.py_backend, NULL) != 0)
        SVC_LOG_WARN("maths_d: python backend unavailable, C path only");
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    atomic_store(&g_svc_maths_d.running, 1);
    return 0;
}

void svc_teardown(void)
{
    atomic_store(&g_svc_maths_d.running, 0);
}

void svc_destroy(void)
{
    maths_backend_destroy(&g_svc_maths_d.py_backend);
    maths_d_service_destroy(&g_svc_maths_d);
}

/* 域调用 → JSON-RPC 响应的唯一出口（三路收敛） */
static void svc_rpc_method(airy_sock_t client_fd, const char *method,
                           cJSON *params, int id)
{
    cJSON *result = NULL;
    char err[256] = "";
    maths_rpc_status_t st = maths_d_rpc_call(&g_svc_maths_d, method, params,
                                             &result, err, sizeof(err));
    if (st == MATHS_RPC_OK) {
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        return;
    }
    if (result)
        cJSON_Delete(result);
    if (st == MATHS_RPC_ERR_METHOD)
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND,
                           "method not found", id);
    else
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_SERVER_ERROR_BASE,
                           err[0] ? err : "internal error", id);
}

/* RPC 壳族：漏斗至 svc_rpc_method 唯一出口，
 * 薄壳机制位由 daemon_main.h DAEMON_RPC_SHELL_M 承载 */
DAEMON_RPC_SHELL_M(health_check, svc_rpc_method)
DAEMON_RPC_SHELL_M(get_stats, svc_rpc_method)
DAEMON_RPC_SHELL_M(recognize, svc_rpc_method)
DAEMON_RPC_SHELL_M(eval, svc_rpc_method)
DAEMON_RPC_SHELL_M(stats, svc_rpc_method)
DAEMON_RPC_SHELL_M(plot, svc_rpc_method)
DAEMON_RPC_SHELL_M(solve, svc_rpc_method)
DAEMON_RPC_SHELL_M(differentiate, svc_rpc_method)
DAEMON_RPC_SHELL_M(integrate, svc_rpc_method)
DAEMON_RPC_SHELL_M(limit, svc_rpc_method)
DAEMON_RPC_SHELL_M(simplify, svc_rpc_method)
DAEMON_RPC_SHELL_M(factor, svc_rpc_method)
DAEMON_RPC_SHELL_M(expand, svc_rpc_method)
DAEMON_RPC_SHELL_M(matrix, svc_rpc_method)
DAEMON_RPC_SHELL_M(units, svc_rpc_method)
DAEMON_RPC_SHELL_M(numerical, svc_rpc_method)
DAEMON_RPC_SHELL_M(finance, svc_rpc_method)
DAEMON_RPC_SHELL_M(number_theory, svc_rpc_method)
