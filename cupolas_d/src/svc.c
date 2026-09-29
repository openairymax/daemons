/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief cupolas_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 安全穹顶 PDP 本体 + daemon 配置解析链 + 生命周期五钩子。配置链：
 * 缺省值 → config 文件 daemon 段覆盖；端点四元组由 svc_endpoint 从
 * 解析结果回填，cmdline use_tcp 只升不降。穹顶引导
 * （daemon_cupolas_init，manifest cupolas:"full"）由生成 main.c 承担；
 * 本文件持有动态策略引擎（PDP，M2-S3 唯一策略持有者）与 cupolas
 * 服务单例的创建、注入与销毁。业务逻辑在 service.c 与 cupolas_rpc_*.c。
 */

#include "svc_cupolas_d.h"

#include "cupolas_d_internal.h"
#include "airy_memory.h"
#include "dynamic_policy_engine.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cupolas_service_t *g_service = NULL;
dpolicy_engine_t *g_dpolicy = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
} cupolas_daemon_config_t;

static cupolas_daemon_config_t g_config = {0};

#define CUPOLAS_MAX_CLIENTS 64

static void config_load(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = CUPOLAS_MAX_CLIENTS;
#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(CUPOLAS_D_SOCKET_WIN);
#else
    g_config.socket_path = AIRY_STRDUP(CUPOLAS_D_SOCKET_UNIX);
#endif
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
    g_config.tcp_port = CUPOLAS_D_TCP_PORT;

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
                    if (!daemon_cfg)
                        break;
                    cJSON *item = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                    if (cJSON_IsString(item)) {
                        AIRY_FREE(g_config.socket_path);
                        g_config.socket_path = AIRY_STRDUP(item->valuestring);
                    }
                    item = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                    if (cJSON_IsNumber(item)) {
                        g_config.tcp_port = (uint16_t)item->valueint;
                        g_config.use_tcp = 1;
                    }
                    item = cJSON_GetObjectItem(daemon_cfg, "max_clients");
                    if (cJSON_IsNumber(item))
                        g_config.max_clients = item->valueint;
                } while (0);
            }
            AIRY_FREE(content);
        }
    }
    fclose(f);
}

static void config_free(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    AIRY_MEMSET(&g_config, 0, sizeof(g_config));
}

void svc_endpoint_cupolas_d(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp || g_config.use_tcp;
    ep->tcp_host = g_config.tcp_host;
    ep->tcp_port = (int)g_config.tcp_port;
    ep->sock_unix = g_config.socket_path;
    ep->sock_win = g_config.socket_path;
}

int svc_prepare_cupolas_d(const char *config_path)
{
    airy_paths_init();
    config_load(config_path);
    SVC_LOG_INFO("Cupolas service starting, manager=%s",
                 config_path ? config_path : "default");

    /* PDP：动态策略引擎（M2-S3）——cupolas_d 作为唯一策略持有者 */
    g_dpolicy = dpolicy_engine_create(DPOLICY_CONFLICT_DENY_WINS);
    if (!g_dpolicy) {
        SVC_LOG_ERROR("Failed to create dynamic policy engine");
        return -1;
    }

    g_service = cupolas_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create cupolas service");
        dpolicy_engine_destroy(g_dpolicy);
        g_dpolicy = NULL;
        return -1;
    }

    /* M2-S2（0.1.9 §3.2 PDP）：PDP 引擎注入服务层——check_permission
     * 裁决先经动态策略运行集（命中即权威），未命中回退基础 ACL。 */
    cupolas_set_dpolicy(g_service, g_dpolicy);
    return 0;
}

int svc_activate_cupolas_d(daemon_event_driver_t *driver)
{
    (void)driver;
    return 0;
}

void svc_teardown_cupolas_d(void)
{
}

void svc_destroy_cupolas_d(void)
{
    if (g_service) {
        cupolas_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_dpolicy) {
        dpolicy_engine_destroy(g_dpolicy);
        g_dpolicy = NULL;
    }
    config_free();
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach_cupolas_d(void *dispatcher)
{
    (void)dispatcher;
}
