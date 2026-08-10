// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
/**
 * @file service.c
 * @brief 网关服务核心实现
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "gateway_service.h"
#include "daemon_platform_ext.h"
#ifdef GATEWAY_HAS_HTTP
#include "http_gateway.h"
#endif
#ifdef GATEWAY_HAS_WS
#include "ws_gateway.h"
#endif
/* stdio 网关不依赖外部库（gateway_lib_obj 恒定编译），无需宏保护 */
#include "stdio_gateway.h"
#ifdef GATEWAY_HAS_HTTP2
#include "http2_gateway.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

typedef enum {
    GW_STATE_CREATED = 0,
    GW_STATE_INITIALIZED,
    GW_STATE_RUNNING,
    GW_STATE_STOPPED
} gw_state_t;

struct gateway_service_s {
    gateway_service_config_t config;
    gw_state_t state;
    uint64_t requests_total;
    uint64_t requests_failed;
#ifdef GATEWAY_HAS_HTTP
    gateway_t *http_gateway;
#endif
#ifdef GATEWAY_HAS_WS
    gateway_t *ws_gateway;
#endif
    gateway_t *stdio_gateway;
    airy_thread_t stdio_thread;         /**< stdio 网关事件循环线程（start 为阻塞循环） */
    int stdio_thread_started;           /**< stdio 线程是否已创建 */
#ifdef GATEWAY_HAS_HTTP2
    gateway_t *http2_gateway;
#endif
    gateway_service_handler_t handler; /**< 业务请求处理器 */
    void *handler_data;                /**< 业务处理器用户数据 */
};

/* stdio 网关 start 为阻塞事件循环（select 循环），放入独立线程运行，
 * 避免阻塞 HTTP/WS/HTTP2 等其余传输的启动。 */
static void *gateway_stdio_thread_main(void *arg)
{
    gateway_t *gw = (gateway_t *)arg;
    if (gw) {
        gateway_start(gw);
    }
    return NULL;
}

void gateway_service_get_default_config(gateway_service_config_t *config)
{
    if (!config)
        return;
    AIRY_MEMSET(config, 0, sizeof(*config));

    config->name = "agentrt-gateway";
    config->version = "0.1.1";

    config->http.type = GATEWAY_DAEMON_TYPE_HTTP;
    config->http.host = "0.0.0.0";
    config->http.port = 8080;
    config->http.enabled = true;
    config->http.max_request_size = 1048576;
    config->http.timeout_ms = 30000;

    config->ws.type = GATEWAY_DAEMON_TYPE_WS;
    config->ws.host = "0.0.0.0";
    config->ws.port = 8081;
    config->ws.enabled = true;
    config->ws.max_request_size = 1048576;
    config->ws.timeout_ms = 30000;

    config->stdio.type = GATEWAY_DAEMON_TYPE_STDIO;
    /* stdio 传输默认关闭：仅当显式传入 -s（或配置文件启用）时才启动。
     * 若默认开启，gateway_d 以 -h/-p TCP 模式运行时 stdin 往往被重定向
     * （/dev/null 或已关闭管道），select 恒可读导致 stdio 线程 100% CPU 忙循环。 */
    config->stdio.enabled = false;
    config->stdio.max_request_size = 1048576;
    config->stdio.timeout_ms = 30000;

    config->enable_metrics = true;
    config->enable_tracing = false;
    config->shutdown_timeout_ms = 5000;
}

airy_err_t gateway_service_load_config(gateway_service_config_t *config,
                                            const char *config_path)
{
    if (!config)
        return AIRY_EINVAL;
    gateway_service_get_default_config(config);
    if (!config_path || config_path[0] == '\0')
        return AIRY_SUCCESS;

    FILE *f = fopen(config_path, "r");
    if (!f)
        return AIRY_SUCCESS;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr)
            *cr = '\0';

        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "http.port") == 0) {
            config->http.port = (uint16_t)strtol(val, NULL, 10);
        } else if (strcmp(key, "http.host") == 0) {
            /* 默认值为字符串字面量 "0.0.0.0"（gateway_service_get_default_config），
             * 直接释放属 UB（free 非堆内存）。仅当已被配置文件替换为堆分配值
             * （非默认字面量）时才释放，与 gateway_svc_adapter.c 的保护写法一致。 */
            if (config->http.host && strcmp(config->http.host, "0.0.0.0") != 0)
                AIRY_FREE((void *)config->http.host);
            config->http.host = AIRY_STRDUP(val);
        } else if (strcmp(key, "http.enabled") == 0) {
            config->http.enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "stdio.max_request_size") == 0) {
            config->stdio.max_request_size = (size_t)atol(val);
        } else if (strcmp(key, "stdio.timeout_ms") == 0) {
            config->stdio.timeout_ms = (uint32_t)strtol(val, NULL, 10);
        } else if (strcmp(key, "enable_metrics") == 0) {
            config->enable_metrics = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "enable_tracing") == 0) {
            config->enable_tracing = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "shutdown_timeout_ms") == 0) {
            config->shutdown_timeout_ms = (uint32_t)strtol(val, NULL, 10);
        }
    }

    fclose(f);
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_create(gateway_service_t *service,
                                       const gateway_service_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    gateway_service_t svc = (gateway_service_t)AIRY_CALLOC(1, sizeof(struct gateway_service_s));
    if (!svc) {
        LOG_ERROR("service allocation failed, size=%zu", sizeof(struct gateway_service_s));
        return AIRY_ENOMEM;
    }
    if (config) {
        AIRY_MEMCPY(&svc->config, config, sizeof(gateway_service_config_t));
    } else {
        gateway_service_get_default_config(&svc->config);
    }
    svc->state = GW_STATE_CREATED;
    *service = svc;
    return AIRY_SUCCESS;
}

void gateway_service_destroy(gateway_service_t service)
{
    if (!service)
        return;
    if (service->state == GW_STATE_RUNNING) {
        gateway_service_stop(service, true);
    }
#ifdef GATEWAY_HAS_HTTP
    if (service->http_gateway) {
        gateway_destroy(service->http_gateway);
    }
#endif
#ifdef GATEWAY_HAS_WS
    if (service->ws_gateway) {
        gateway_destroy(service->ws_gateway);
    }
#endif
    if (service->stdio_gateway) {
        gateway_destroy(service->stdio_gateway);
    }
#ifdef GATEWAY_HAS_HTTP2
    if (service->http2_gateway) {
        gateway_destroy(service->http2_gateway);
    }
#endif
    AIRY_FREE(service);
}

airy_err_t gateway_service_init(gateway_service_t service)
{
    if (!service)
        return AIRY_EINVAL;
    if (service->state != GW_STATE_CREATED)
        return AIRY_EPERM;
    service->state = GW_STATE_INITIALIZED;
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_start(gateway_service_t service)
{
    if (!service)
        return AIRY_EINVAL;
    if (service->state == GW_STATE_RUNNING)
        return AIRY_SUCCESS;
    if (service->state != GW_STATE_INITIALIZED && service->state != GW_STATE_STOPPED) {
        LOG_ERROR("service start rejected: invalid state=%d", service->state);
        return AIRY_EPERM;
    }
    service->state = GW_STATE_RUNNING;

#ifdef GATEWAY_HAS_HTTP
    if (service->config.http.enabled) {
        service->http_gateway =
            http_gateway_create(service->config.http.host, service->config.http.port);
        if (!service->http_gateway) {
            LOG_ERROR("http_gateway_create failed: host=%s, port=%d",
                              service->config.http.host, service->config.http.port);
            service->state = GW_STATE_STOPPED;
            return AIRY_ENOMEM;
        }
        /* 注册业务处理器：未注册时 HTTP JSON-RPC 请求会因 handler 为 NULL 失败 */
        if (service->handler && service->http_gateway->ops &&
            service->http_gateway->ops->set_handler) {
            service->http_gateway->ops->set_handler(service->http_gateway->impl,
                                                    service->handler, service->handler_data);
        }
        gateway_start(service->http_gateway);
    }
#endif

#ifdef GATEWAY_HAS_WS
    /* AIRY_GATEWAY_DISABLE_WS=1 可显式关闭 WebSocket 传输（诊断/受限环境用） */
    const char *ws_disable_env = getenv("AIRY_GATEWAY_DISABLE_WS");
    int ws_disabled = ws_disable_env && strcmp(ws_disable_env, "1") == 0;
    if (service->config.ws.enabled && !ws_disabled) {
        service->ws_gateway =
            ws_gateway_create(service->config.ws.host, service->config.ws.port);
        if (!service->ws_gateway) {
            /* 创建失败只记日志，不阻塞其余传输（WS 为可选传输） */
            LOG_ERROR("ws_gateway_create failed: host=%s, port=%d",
                              service->config.ws.host, service->config.ws.port);
        } else {
            if (service->handler && service->ws_gateway->ops &&
                service->ws_gateway->ops->set_handler) {
                service->ws_gateway->ops->set_handler(service->ws_gateway->impl,
                                                      service->handler, service->handler_data);
            }
            if (gateway_start(service->ws_gateway) == AIRY_SUCCESS) {
                LOG_INFO("WS gateway started on %s:%d", service->config.ws.host,
                         service->config.ws.port);
            } else {
                LOG_ERROR("WS gateway start failed on %s:%d", service->config.ws.host,
                          service->config.ws.port);
            }
        }
    } else if (!service->config.ws.enabled) {
        LOG_INFO("WS gateway disabled by config");
    }
#endif

    if (service->config.stdio.enabled) {
        service->stdio_gateway = stdio_gateway_create();
        if (!service->stdio_gateway) {
            LOG_ERROR("stdio_gateway_create failed");
        } else {
            if (service->handler && service->stdio_gateway->ops &&
                service->stdio_gateway->ops->set_handler) {
                service->stdio_gateway->ops->set_handler(service->stdio_gateway->impl,
                                                         service->handler,
                                                         service->handler_data);
            }
            /* stdio start 为阻塞事件循环：放入独立线程，不阻塞其余传输 */
            if (airy_thread_create(&service->stdio_thread, gateway_stdio_thread_main,
                                   service->stdio_gateway) != 0) {
                LOG_ERROR("stdio gateway thread create failed");
                gateway_destroy(service->stdio_gateway);
                service->stdio_gateway = NULL;
            } else {
                service->stdio_thread_started = 1;
                LOG_INFO("Stdio gateway started");
            }
        }
    }

#ifdef GATEWAY_HAS_HTTP2
    if (service->config.http.enabled) {
        /* HTTP/2 需要独立监听端口（默认 http.port+2，可用 AIRY_GATEWAY_HTTP2_PORT
         * 覆盖），避免与 HTTP/1.1 网关同端口 bind 冲突 */
        const char *h2port_env = getenv("AIRY_GATEWAY_HTTP2_PORT");
        uint16_t h2_port = (h2port_env && *h2port_env)
            ? (uint16_t)atoi(h2port_env)
            : (uint16_t)(service->config.http.port + 2);
        service->http2_gateway =
            http2_gateway_create(service->config.http.host, h2_port);
        if (!service->http2_gateway) {
            LOG_ERROR("http2_gateway_create failed: host=%s, port=%d",
                              service->config.http.host, h2_port);
        } else {
            if (service->handler && service->http2_gateway->ops &&
                service->http2_gateway->ops->set_handler) {
                service->http2_gateway->ops->set_handler(service->http2_gateway->impl,
                                                         service->handler,
                                                         service->handler_data);
            }
            if (gateway_start(service->http2_gateway) == AIRY_SUCCESS) {
                LOG_INFO("HTTP/2 gateway started on %s:%d", service->config.http.host,
                         h2_port);
            } else {
                LOG_ERROR("HTTP/2 gateway start failed on %s:%d", service->config.http.host,
                          h2_port);
            }
        }
    }
#endif
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_set_handler(gateway_service_t service,
                                       gateway_service_handler_t handler,
                                       void *user_data)
{
    if (!service)
        return AIRY_EINVAL;
    if (service->state == GW_STATE_RUNNING) {
        LOG_ERROR("gateway_service_set_handler rejected: service running");
        return AIRY_EPERM;
    }
    service->handler = handler;
    service->handler_data = user_data;
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_stop(gateway_service_t service, bool force __attribute__((unused)))
{
    if (!service)
        return AIRY_EINVAL;
    if (service->state != GW_STATE_RUNNING)
        return AIRY_SUCCESS;
    LOG_INFO("gateway_service_stop: begin (http/ws/stdio/http2 teardown)");
#ifdef GATEWAY_HAS_HTTP
    if (service->http_gateway) {
        gateway_destroy(service->http_gateway);
        service->http_gateway = NULL;
        LOG_INFO("gateway_service_stop: HTTP gateway destroyed");
    }
#endif
#ifdef GATEWAY_HAS_WS
    if (service->ws_gateway) {
        gateway_destroy(service->ws_gateway);
        service->ws_gateway = NULL;
        LOG_INFO("gateway_service_stop: WS gateway destroyed");
    }
#endif
    if (service->stdio_gateway) {
        gateway_destroy(service->stdio_gateway); /* 内部置 running=false，事件循环 1s 内退出 */
        service->stdio_gateway = NULL;
        if (service->stdio_thread_started) {
            airy_thread_join(service->stdio_thread, NULL);
            service->stdio_thread_started = 0;
        }
        LOG_INFO("gateway_service_stop: Stdio gateway destroyed");
    }
#ifdef GATEWAY_HAS_HTTP2
    if (service->http2_gateway) {
        gateway_destroy(service->http2_gateway);
        service->http2_gateway = NULL;
        LOG_INFO("gateway_service_stop: HTTP/2 gateway destroyed");
    }
#endif
    service->state = GW_STATE_STOPPED;
    LOG_INFO("gateway_service_stop: complete");
    return AIRY_SUCCESS;
}

airy_svc_state_t gateway_service_get_state(gateway_service_t service)
{
    if (!service)
        return AIRY_SVC_STATE_NONE;
    switch (service->state) {
    case GW_STATE_CREATED:
        return AIRY_SVC_STATE_CREATED;
    case GW_STATE_INITIALIZED:
        return AIRY_SVC_STATE_READY;
    case GW_STATE_RUNNING:
        return AIRY_SVC_STATE_RUNNING;
    case GW_STATE_STOPPED:
        return AIRY_SVC_STATE_STOPPED;
    default:
        return AIRY_SVC_STATE_ERROR;
    }
}

bool gateway_service_is_running(gateway_service_t service)
{
    if (!service)
        return false;
    return service->state == GW_STATE_RUNNING;
}

airy_err_t gateway_service_get_stats(gateway_service_t service, airy_svc_stats_t *stats)
{
    if (!service || !stats)
        return AIRY_EINVAL;
    AIRY_MEMSET(stats, 0, sizeof(*stats));
    stats->request_count = service->requests_total;
    stats->error_count = service->requests_failed;
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_healthcheck(gateway_service_t service)
{
    if (!service)
        return AIRY_EINVAL;
    return (service->state == GW_STATE_RUNNING) ? AIRY_SUCCESS : AIRY_EALREADY;
}
