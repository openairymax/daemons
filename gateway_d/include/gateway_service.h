/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_service.h
 * @brief Gateway daemon service interface.
 *
 * gateway_d is AgentRT's gateway daemon, responsible for:
 * 1. Managing HTTP/WebSocket/Stdio gateway instances
 * 2. Providing unified configuration management
 * 3. Implementing service lifecycle management
 * 4. Interfacing with the syscall layer
 *
 * Architecture:
 *   gateway_d/ -> agentrt/gateway/ -> agentrt/atoms/syscall/
 *       ^           ^
 *    daemon      protocol-translation layer
 *
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_SERVICE_H
#define AIRY_RT_DAEMON_GATEWAY_SERVICE_H

#include "svc_common.h"

#include <stdbool.h>
#include <stdint.h>

/* 版本 SSoT：agentrt 全系统版本单一权威源在 daemons/common/include/
 * airyrt_version.h（与 VERSION 文件、根 CMakeLists project() 同步）。
 * 其余模块（CLI/协议/MCP server/各 svc_adapter）一律引用 AIRYRT_VERSION，
 * 禁止散落硬编码版本串。 */
#include "airyrt_version.h"

#ifdef __cplusplus
extern "C" {
#endif


/** @brief Gateway type enum. */
typedef enum {
    GATEWAY_DAEMON_TYPE_HTTP = 0,
    GATEWAY_DAEMON_TYPE_WS,
    GATEWAY_DAEMON_TYPE_STDIO
} gateway_daemon_type_t;


/** @brief Single gateway config. */
typedef struct {
    gateway_daemon_type_t type;
    const char *host;
    uint16_t port;
    bool enabled;
    size_t max_request_size;
    uint32_t timeout_ms;
} gateway_daemon_config_t;

/** @brief Gateway service config. */
typedef struct {
    const char *name;
    const char *version;

    gateway_daemon_config_t http;
    gateway_daemon_config_t ws;
    gateway_daemon_config_t stdio;
    bool enable_metrics;
    bool enable_tracing;
    uint32_t shutdown_timeout_ms;
} gateway_service_config_t;


/** @brief Gateway service handle. */
typedef struct gateway_service_s *gateway_service_t;


/**
 * @brief Create a gateway service.
 * @param[out] service Service-handle output
 * @param[in] config Service config
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_create(gateway_service_t *service,
                                           const gateway_service_config_t *config);

/**
 * @brief Destroy a gateway service.
 * @param[in] service Service handle
 */
AIRY_API void gateway_service_destroy(gateway_service_t service);

/**
 * @brief Initialize a gateway service.
 * @param[in] service Service handle
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_init(gateway_service_t service);

/**
 * @brief Start a gateway service.
 * @param[in] service Service handle
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_start(gateway_service_t service);

/**
 * @brief Stop a gateway service.
 * @param[in] service Service handle
 * @param[in] force Whether to force stop
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_stop(gateway_service_t service, bool force);


/**
 * @brief Get the service state.
 * @param[in] service Service handle
 * @return Service state
 */
AIRY_API airy_svc_state_t gateway_service_get_state(gateway_service_t service);

/**
 * @brief Check whether the service is running.
 * @param[in] service Service handle
 * @return true if running
 */
AIRY_API bool gateway_service_is_running(gateway_service_t service);

/**
 * @brief Get service statistics.
 * @param[in] service Service handle
 * @param[out] stats Statistics output
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_get_stats(gateway_service_t service, airy_svc_stats_t *stats);

/**
 * @brief Run a health check.
 * @param[in] service Service handle
 * @return AIRY_SUCCESS if healthy
 */
AIRY_API airy_err_t gateway_service_healthcheck(gateway_service_t service);


/**
 * @brief Load config from a config file.
 * @param[out] config Config output
 * @param[in] config_path Config file path
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_load_config(gateway_service_config_t *config,
                                                const char *config_path);

/**
 * @brief Get the default config.
 * @param[out] config Config output
 */
AIRY_API void gateway_service_get_default_config(gateway_service_config_t *config);


/**
 * @brief Gateway business-request handler (internal signature, aligned with
 *        the http_gateway underlying handler).
 *
 * @param request Standard JSON-RPC request string (UTF-8, normalized by the
 *                multi-protocol handler)
 * @param user_data User data passed at registration
 * @return JSON response string (AIRY_MALLOC-allocated, caller frees), NULL on failure
 */
typedef char *(*gateway_service_handler_t)(void *request, void *user_data);

/**
 * @brief Register the gateway business-request handler.
 *
 * Without registration, HTTP JSON-RPC requests return "Custom handler failed"
 * (handler is NULL). Must be called before gateway_service_start() to take
 * effect.
 *
 * @param[in] service Service handle
 * @param[in] handler Business handler
 * @param[in] user_data User data (passed through to the handler)
 * @return AIRY_SUCCESS on success
 */
AIRY_API airy_err_t gateway_service_set_handler(gateway_service_t service,
                                                gateway_service_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_SERVICE_H */
