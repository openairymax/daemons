/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file gateway_svc_adapter.h
 * @brief Gateway service-adapter header.
 *
 * Provides the adapter interface between the gateway service and the
 * AgentRT unified service-management framework. Through this adapter the
 * gateway service integrates seamlessly into the service registry, service
 * discovery and unified lifecycle management.
 *
 * Design principles:
 * 1. Interface contracts - standardized service interface
 * 2. Backward compatibility - stays compatible with existing gateway services
 * 3. Minimal intrusion - minimal impact on original service code
 * 4. Extensibility - supports future service-feature extension
 *
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_SVC_ADAPTER_H
#define AIRY_RT_DAEMON_GATEWAY_SVC_ADAPTER_H

#include "gateway_service.h"
#include "svc_common.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Create a gateway service adapter.
 *
 * Creates a new gateway service-adapter instance implementing the
 * airy_svc_t interface, manageable through the unified AgentRT
 * service-management framework.
 *
 * @param[out] out_service Output service handle
 * @param[in] config Generic service config (may be NULL = default config)
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no (call in a single-threaded context)
 * @reentrant yes
 *
 * @example
 * @code
 * airy_svc_t svc = NULL;
 * airy_svc_config_t config = {
 *     .name = "gateway_service",
 *     .version = "1.0.0",
 *     .capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_STREAMING,
 *     .max_concurrent = 1000,
 *     .timeout_ms = 30000,
 *     .auto_start = true,
 *     .enable_metrics = true
 * };
 *
 * airy_err_t err = gateway_service_adapter_create(&svc, &config);
 * if (err == AIRY_SUCCESS) {
 *
 *     airy_svc_init(svc);
 *     airy_svc_start(svc);
 * }
 * @endcode
 */
AIRY_API airy_err_t gateway_service_adapter_create(airy_svc_t *out_service,
                                                   const airy_svc_config_t *config);

/**
 * @brief Wrap an existing gateway service as an adapter.
 *
 * Wraps an existing gateway service instance as an adapter so it can be
 * integrated into the service-management framework. After wrapping, the
 * original service handle is managed by the adapter and should not be used
 * directly.
 *
 * @param[out] out_service Output service handle
 * @param[in] gateway_svc Original gateway service handle
 * @param[in] config Generic service config (may be NULL = default config)
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @warning After wrapping, the original service handle's lifecycle is
 *          managed by the adapter; do not call gateway_service_destroy etc.
 *
 * @threadsafe no
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_wrap(airy_svc_t *out_service,
                                                 gateway_service_t gateway_svc,
                                                 const airy_svc_config_t *config);

/**
 * @brief Get the original gateway service handle.
 *
 * For scenarios needing direct access to gateway-service-specific features.
 * The returned handle must not be modified or destroyed; the adapter
 * manages its lifecycle.
 *
 * @param service Adapter service handle
 * @return Original gateway service handle, or NULL (if invalid)
 *
 * @threadsafe yes (provided service state is unchanged)
 * @reentrant yes
 */
AIRY_API gateway_service_t gateway_service_adapter_get_original(airy_svc_t service);


/**
 * @brief Initialize the adapter service.
 *
 * Initializes the adapter service, creating the underlying gateway service
 * instance. No-op if the adapter was created via gateway_service_adapter_wrap.
 *
 * @param service Adapter service handle
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no
 * @reentrant no
 */
AIRY_API airy_err_t gateway_service_adapter_init(airy_svc_t service);

/**
 * @brief Start the adapter service.
 *
 * Starts the underlying gateway service.
 *
 * @param service Adapter service handle
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no
 * @reentrant no
 */
AIRY_API airy_err_t gateway_service_adapter_start(airy_svc_t service);

/**
 * @brief Stop the adapter service.
 *
 * Stops the underlying gateway service.
 *
 * @param service Adapter service handle
 * @param force Whether to force stop
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe yes (the underlying gateway service must support concurrent stop)
 * @reentrant no
 */
AIRY_API airy_err_t gateway_service_adapter_stop(airy_svc_t service, bool force);

/**
 * @brief Destroy the adapter service.
 *
 * Destroys the adapter and the underlying gateway service it manages.
 *
 * @param service Adapter service handle
 *
 * @threadsafe no
 * @reentrant no
 */
AIRY_API void gateway_service_adapter_destroy(airy_svc_t service);

/**
 * @brief Health check of the adapter service.
 *
 * Runs the health check of the underlying gateway service.
 *
 * @param service Adapter service handle
 * @return AIRY_SUCCESS healthy, other values unhealthy
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_healthcheck(airy_svc_t service);


/**
 * @brief Get the adapter service state.
 *
 * Gets the current state of the underlying gateway service.
 *
 * @param service Adapter service handle
 * @return Service-state enum value
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API airy_svc_state_t gateway_service_adapter_get_state(airy_svc_t service);

/**
 * @brief Check whether the adapter service is running.
 *
 * Checks whether the underlying gateway service is in the running state.
 *
 * @param service Adapter service handle
 * @return true running, false not running
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API bool gateway_service_adapter_is_running(airy_svc_t service);

/**
 * @brief Get the adapter service statistics.
 *
 * Gets the statistics of the underlying gateway service.
 *
 * @param service Adapter service handle
 * @param stats Statistics output
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_get_stats(airy_svc_t service, airy_svc_stats_t *stats);


/**
 * @brief Create the gateway service-adapter interface.
 *
 * Returns the standard adapter interface of the gateway service, usable
 * with airy_svc_create. This is mainly for advanced usage; usually
 * gateway_service_adapter_create suffices.
 *
 * @return Gateway service-adapter interface struct
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API const airy_svc_interface_t *gateway_service_adapter_get_interface(void);

/**
 * @brief Check whether a specific gateway type is supported.
 *
 * Checks whether the adapter supports the given gateway type.
 *
 * @param service Adapter service handle
 * @param type Gateway type
 * @return true supported, false not supported
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API bool gateway_service_adapter_supports_type(airy_svc_t service, gateway_daemon_type_t type);

/**
 * @brief Enable/disable a specific gateway type.
 *
 * Dynamically enables or disables a gateway type. Callable only while the
 * service is stopped.
 *
 * @param service Adapter service handle
 * @param type Gateway type
 * @param enabled Whether enabled
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no
 * @reentrant no
 */
AIRY_API airy_err_t gateway_service_adapter_set_type_enabled(airy_svc_t service,
                                                             gateway_daemon_type_t type,
                                                             bool enabled);


/**
 * @brief Create a gateway service adapter from a config file.
 *
 * Loads the config from a file and creates the gateway service adapter.
 *
 * @param[out] out_service Output service handle
 * @param config_path Config-file path
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_create_from_config(airy_svc_t *out_service,
                                                               const char *config_path);

/**
 * @brief Reload the adapter config.
 *
 * Reloads the config from a file and updates the adapter. Callable only
 * while the service is stopped.
 *
 * @param service Adapter service handle
 * @param config_path Config-file path
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe no
 * @reentrant no
 */
AIRY_API airy_err_t gateway_service_adapter_reload_config(airy_svc_t service,
                                                          const char *config_path);


/**
 * @brief Register the gateway service adapter with the service registry.
 *
 * Registers the gateway service adapter in the global service registry,
 * making it discoverable via service discovery.
 *
 * @param service Adapter service handle
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_register(airy_svc_t service);

/**
 * @brief Unregister the gateway service adapter from the service registry.
 *
 * Unregisters the gateway service adapter from the global service registry.
 *
 * @param service Adapter service handle
 * @return AIRY_SUCCESS on success, other values are error codes
 *
 * @threadsafe yes
 * @reentrant yes
 */
AIRY_API airy_err_t gateway_service_adapter_unregister(airy_svc_t service);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_SVC_ADAPTER_H */