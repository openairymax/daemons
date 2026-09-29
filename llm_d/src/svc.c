/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief llm_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * llm 服务单例 + 生命周期五钩子。可配置户：端点基线取自生成头常量
 * （LLM_D_SOCKET_UNIX/WIN、LLM_D_TCP_PORT），-c JSON 的 daemon 段可覆盖
 * socket_path / tcp_port；cmdline use_tcp 只升不降。配置未显式给出时回落
 * $AIRY_CONFIG_DIR/model.yaml（与 think_d / gateway_d 同源 SSoT），使
 * provider 注册表与 llm_router 始终看到已配置端点。业务逻辑在
 * src/llm_rpc.c 与 src/llm_rpc_request.c。
 */

#include "daemon_main.h"
#include "platform.h"
#include "svc_logger.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"
#include "llm_service.h"

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>

llm_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    uint16_t tcp_port;
    int use_tcp;
} llm_ep_cfg_t;

static llm_ep_cfg_t g_ep = {0};

static void ep_load(const char *config_path)
{
    g_ep.use_tcp = 0;
    g_ep.tcp_port = LLM_D_TCP_PORT;
#if defined(AIRY_PLATFORM_WINDOWS)
    g_ep.socket_path = AIRY_STRDUP(LLM_D_SOCKET_WIN);
#else
    g_ep.socket_path = AIRY_STRDUP(LLM_D_SOCKET_UNIX);
#endif

    if (!config_path)
        return;

    FILE *f = fopen(config_path, "rb");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 1024 * 1024) {
        char *content = (char *)AIRY_MALLOC((size_t)len + 1);
        if (content) {
            size_t read_len = fread(content, 1, (size_t)len, f);
            if (read_len == (size_t)len) {
                content[read_len] = '\0';
                do {
                    CJSON_PARSE_GUARD(root, content, { break; });
                    cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                    if (daemon_cfg) {
                        cJSON *socket_path = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                        if (cJSON_IsString(socket_path)) {
                            AIRY_FREE(g_ep.socket_path);
                            g_ep.socket_path = AIRY_STRDUP(socket_path->valuestring);
                        }
                        cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                        if (cJSON_IsNumber(tcp_port)) {
                            g_ep.tcp_port = (uint16_t)tcp_port->valueint;
                            g_ep.use_tcp = 1;
                        }
                    }
                } while (0);
            }
            AIRY_FREE(content);
        }
    }
    fclose(f);
}

static void ep_free(void)
{
    AIRY_FREE(g_ep.socket_path);
    AIRY_MEMSET(&g_ep, 0, sizeof(g_ep));
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
    ep->use_tcp = cmdline_tcp ? 1 : (g_ep.use_tcp ? 1 : 0);
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = g_ep.tcp_port;
    ep->sock_unix = g_ep.socket_path;
    ep->sock_win = g_ep.socket_path;

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
