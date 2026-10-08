/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief channel_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * daemon 配置解析链 + channel 服务生命周期五钩子。配置链：缺省值
 * （CHANNEL_CONFIG_DEFAULTS）→ config 文件 daemon 段覆盖
 * （socket_dir / max_channels；原 -s/-n 命令行参数已迁移至配置面，
 * 机制统一走 --manager）。端点由 svc_endpoint 从生成头常量回填，
 * cmdline use_tcp 只升不降。channel 服务单例的创建、启动与销毁由
 * 本文件持有；业务逻辑在 channel_service.c / channel_io.c 与
 * channel_rpc.c。
 */

#include "svc_channel_d.h"

#include "airy_memory.h"
#include "channel_d_internal.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

channel_service_t *g_svc = NULL;

static channel_config_t g_cfg = CHANNEL_CONFIG_DEFAULTS;

static void cfg_load(const char *config_path)
{
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
                    cJSON *item = cJSON_GetObjectItem(daemon_cfg, "socket_dir");
                    if (cJSON_IsString(item) && item->valuestring[0]) {
                        AIRY_STRNCPY_TERM(g_cfg.socket_dir, item->valuestring,
                                          sizeof(g_cfg.socket_dir));
                    }
                    item = cJSON_GetObjectItem(daemon_cfg, "max_channels");
                    if (cJSON_IsNumber(item) && item->valueint > 0 &&
                        item->valueint <= CHANNEL_MAX_CHANNELS)
                        g_cfg.max_channels = (uint32_t)item->valueint;
                } while (0);
            }
            AIRY_FREE(content);
        }
    }
    fclose(f);
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_base(ep, cmdline_tcp, CHANNEL_D_SOCKET_UNIX,
                   CHANNEL_D_SOCKET_WIN, CHANNEL_D_TCP_PORT);
}

int svc_prepare(const char *config_path)
{
    airy_paths_init();
    cfg_load(config_path);

    g_svc = channel_service_create(&g_cfg);
    if (!g_svc) {
        SVC_LOG_ERROR("Failed to create channel service");
        return -1;
    }
    if (channel_service_start(g_svc) != 0) {
        SVC_LOG_ERROR("Failed to start channel service");
        channel_service_destroy(g_svc);
        g_svc = NULL;
        return -1;
    }
    SVC_LOG_INFO("channel service started (max_channels=%u, socket_dir=%s)",
                 g_cfg.max_channels, g_cfg.socket_dir);
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_svc) {
        channel_service_stop(g_svc);
        channel_service_destroy(g_svc);
        g_svc = NULL;
    }
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
