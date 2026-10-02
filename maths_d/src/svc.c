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

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_base(ep, cmdline_tcp, MATHS_D_SOCKET_UNIX,
                   MATHS_D_SOCKET_WIN, MATHS_D_TCP_PORT);
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

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
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

/* handler 薄壳：user_data 为机制层注入的 &client_fd
 * （DAEMON_REGISTER_METHODS user_data=NULL 约定） */
#define SVC_RPC_HANDLER(name)                                                  \
    void m_##name(cJSON *params, int id, void *user_data)                      \
    {                                                                          \
        svc_rpc_method(*(airy_sock_t *)user_data, #name, params, id);          \
    }

SVC_RPC_HANDLER(health_check)
SVC_RPC_HANDLER(get_stats)
SVC_RPC_HANDLER(recognize)
SVC_RPC_HANDLER(eval)
SVC_RPC_HANDLER(stats)
SVC_RPC_HANDLER(plot)
SVC_RPC_HANDLER(solve)
SVC_RPC_HANDLER(differentiate)
SVC_RPC_HANDLER(integrate)
SVC_RPC_HANDLER(limit)
SVC_RPC_HANDLER(simplify)
SVC_RPC_HANDLER(factor)
SVC_RPC_HANDLER(expand)
SVC_RPC_HANDLER(matrix)
SVC_RPC_HANDLER(units)
SVC_RPC_HANDLER(numerical)
SVC_RPC_HANDLER(finance)
SVC_RPC_HANDLER(number_theory)
