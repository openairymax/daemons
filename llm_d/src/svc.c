/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief llm_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * llm 服务单例 + 生命周期五钩子。可配置户：端点基线取自生成头常量
 * （LLM_D_SOCKET_UNIX/WIN、LLM_D_TCP_PORT），端点族委托 daemon_cfg_file
 * 机制件（daemon_ep_load/free/fill，纯端点户无策略键）。配置未显式
 * 给出时回落 $AIRY_CONFIG_DIR/model.yaml（与 think_d / gateway_d 同源
 * SSoT），使 provider 注册表与 llm_router 始终看到已配置端点。业务
 * 逻辑在 src/rpc/methods.c 与 src/rpc/dispatch.c。
 */

#include "platform.h"
#include "svc_logger.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"
#include "llm_service.h"

#include "daemon_cfg_file.h"

#include <stdio.h>

llm_service_t *g_service = NULL;

static daemon_ep_cfg_t g_ep = {0};

static void ep_load(const char *config_path)
{
    daemon_ep_load(&g_ep, config_path, LLM_D_SOCKET_UNIX,
                   LLM_D_SOCKET_WIN, LLM_D_TCP_PORT, NULL, NULL);
}

static void ep_free(void)
{
    daemon_ep_free(&g_ep);
}

/* 未显式指定 manager 时回落 $AIRY_CONFIG_DIR/model.yaml：静态缓冲保持
 * 进程生命周期有效（llm_service 不复制该路径）。 */
static const char *cfg_fallback(const char *config_path)
{
    static char default_model_path[1024];
    if (config_path)
        return config_path;

    const char *cfg_dir = airy_config_dir();
    if (!cfg_dir)
        return NULL;

    int plen = snprintf(default_model_path, sizeof(default_model_path), "%s/model.yaml", cfg_dir);
    if (plen <= 0 || plen >= (int)sizeof(default_model_path))
        return NULL;
    return default_model_path;
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_fill(ep, &g_ep, cmdline_tcp);

    if (ep->use_tcp)
        SVC_LOG_INFO("Listening on TCP port %u", (unsigned)ep->tcp_port);
    else
        SVC_LOG_INFO("Listening on %s", ep->sock_unix ? ep->sock_unix : "(unset)");
}

int svc_prepare(const char *config_path)
{
    /* LLM_D_SOCKET_UNIX 展开为 airy_runtime_dir_socket()，且 cfg_fallback
     * 依赖 airy_config_dir()：路径体系须先就绪（与 think_d/gateway_d 同序）。 */
    airy_paths_init();

    const char *cfg = cfg_fallback(config_path);
    ep_load(cfg);

    SVC_LOG_INFO("LLM service starting, manager=%s", cfg ? cfg : "default");

    g_service = llm_service_create(cfg);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create service");
        return -1;
    }
    SVC_LOG_INFO("LLM service started");
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        llm_service_destroy(g_service);
        g_service = NULL;
    }
    ep_free();
}

/* 策略层附加装配挂点：llm_d 的 llm.* 方法面已由生成态静态注册表
 * （SVC_METHODS）全量覆盖，无附加动态注册族，提供空实现以满足契约。 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
