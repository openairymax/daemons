// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file llm_daemon_config.c
 * @brief llm_d daemon config-assembly domain (split from main.c,
 *        2026-08-27): daemon socket/TCP/thread-pool config loading and
 *        process-level service destruction.
 *
 * 2026-08-27 域拆分（main.c 1033 行 → 4 文件）：本文件持有 g_config 定义，
 * 负责从 manager/model.yaml 提取 daemon 段配置并在退出路径销毁服务；
 * 入口引导在 main.c，请求解析与 RPC 方法分别在 llm_daemon_request.c /
 * llm_daemon_methods.c。
 */

#include "airy_memory.h"
#include "llm_service_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* P0.18.1: transitively provides SVC_LOG_*、CJSON_PARSE_GUARD 以及
 * DEFAULT_SOCKET_PATH_UNIX 展开所需的 platform 路径声明。 */
#include "daemon_main.h"

llm_daemon_config_t g_config = {0};

/**
 * @brief Load the daemon config
 */
int load_daemon_config(const char *config_path)
{

    g_config.use_tcp = 0;
    g_config.max_threads = MAX_THREADS;
    g_config.max_clients = MAX_CLIENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);

            char *content = (char *)AIRY_MALLOC(len + 1);
            if (content) {
                size_t nread = fread(content, 1, len, f);
                if (nread == (size_t)len) {
                    content[len] = '\0';

                    /* P0.18.2: CJSON_PARSE_GUARD replaces cJSON_Parse + if
                     * (root) + manual cJSON_Delete, using do { ... } while (0)
                     * + break to preserve the original if (root) block
                     * semantics: skip config extraction on parse failure */
                    do {
                        CJSON_PARSE_GUARD(root, content, { break; });
                        cJSON *daemon = cJSON_GetObjectItem(root, "daemon");
                        if (daemon) {
                            cJSON *socket_path = cJSON_GetObjectItem(daemon, "socket_path");
                            if (cJSON_IsString(socket_path)) {
                                AIRY_FREE(g_config.socket_path);
                                g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                            }

                            cJSON *tcp_port = cJSON_GetObjectItem(daemon, "tcp_port");
                            if (cJSON_IsNumber(tcp_port)) {
                                g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                g_config.use_tcp = 1;
                            }

                            cJSON *max_threads = cJSON_GetObjectItem(daemon, "max_threads");
                            if (cJSON_IsNumber(max_threads)) {
                                g_config.max_threads = max_threads->valueint;
                            }
                        }

                    } while (0);
                }
                AIRY_FREE(content);
            }
            fclose(f);
        }
    }

    return 0;
}

/**
 * @brief Release the config resources
 */
void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

void destroy_service_llm_d(void)
{
    if (g_service) {
        llm_service_destroy(g_service);
        g_service = NULL;
    }
    free_daemon_config();
}
