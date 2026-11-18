/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 *
 * @file network_common.h
 * @brief Network communication module: cross-platform network abstraction
 *        layer.
 *
 * @details
 * This module provides cross-platform network communication, including:
 * - TCP/UDP Socket connections
 * - HTTP/HTTPS client
 * - WebSocket support
 * - Connection pool management
 * - Timeout and retry mechanisms
 *
 * Supported platforms:
 * - Windows (Winsock2)
 * - Linux (POSIX Socket)
 * - macOS (POSIX Socket)
 *
 * @note Thread safety: all public interfaces are thread-safe
 * @see ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency
 */

#ifndef AIRY_RT_NETWORK_COMMON_H
#define AIRY_RT_NETWORK_COMMON_H

#include <error.h>
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_DEFAULT_TIMEOUT_MS 30000

#define NETWORK_DEFAULT_MAX_RETRIES 3

#define NETWORK_DEFAULT_RETRY_INTERVAL 1000

#define NETWORK_DEFAULT_BUFFER_SIZE 8192

#define NETWORK_MAX_POOL_SIZE 32

#define NETWORK_MAGIC 0x4E455457 /* "NETW" */

/**
 * @brief Network connection state enumeration
 */
typedef enum {
    NETWORK_STATUS_DISCONNECTED = 0,
    NETWORK_STATUS_CONNECTING = 1,
    NETWORK_STATUS_CONNECTED = 2,
    NETWORK_STATUS_DISCONNECTING = 3,
    NETWORK_STATUS_ERROR = 4
} network_status_t;

/**
 * @brief Socket type enumeration
 */
typedef enum {
    NETWORK_SOCK_STREAM = 1,
    NETWORK_SOCK_DGRAM = 2,
    NETWORK_SOCK_RAW = 3
} network_sock_type_t;

/**
 * @brief Address family enumeration
 */
typedef enum {
    NETWORK_AF_UNSPEC = 0,
    NETWORK_AF_INET = 2, /**< IPv4 */
    NETWORK_AF_INET6 = 10 /**< IPv6 */
} network_af_t;

/**
 * @brief SSL/TLS verification modes
 */
typedef enum {
    NETWORK_SSL_VERIFY_NONE = 0,
    NETWORK_SSL_VERIFY_PEER = 1,
    NETWORK_SSL_VERIFY_FAIL_IF_NO_PEER_CERT = 2,
    NETWORK_SSL_VERIFY_CLIENT_ONCE = 4
} network_ssl_verify_t;

/**
 * @brief Network configuration structure
 */
typedef struct {
    const char *host;
    int port;
    int timeout_ms;
    int read_timeout_ms;
    int write_timeout_ms;
    int max_retries;
    int retry_interval_ms;
    network_sock_type_t sock_type;
    network_af_t af;
    bool keepalive;
    bool nonblocking;
    bool ssl_enable;
    network_ssl_verify_t ssl_verify;
    const char *ssl_cert_path;
    const char *ssl_key_path;
    const char *ssl_ca_path;
} network_config_t;

/**
 * @brief Network statistics structure
 */
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t connect_count;
    uint64_t error_count;
    uint64_t retry_count;
    uint64_t avg_latency_us;
} network_stats_t;

/**
 * @brief Network connection handle (opaque pointer)
 */
typedef struct network_connection network_connection_t;

/**
 * @brief Connection pool handle (opaque pointer)
 */
typedef struct network_pool network_pool_t;

/**
 * @brief HTTP request structure
 */
typedef struct {
    const char *method;
    const char *path;
    const char *content_type; /**< Content-Type */
    const void *body;
    size_t body_len;
    const char **headers;
    size_t header_count;
    int timeout_ms;
    bool follow_redirects;
    int max_redirects;
} network_http_request_t;

/**
 * @brief HTTP response structure
 */
typedef struct {
    int status_code;
    char *status_text;
    char **headers;
    size_t header_count;
    void *body;
    size_t body_len;
    airy_err_t error;
    char *error_message;
    uint64_t latency_us;
} network_http_response_t;

/**
 * @brief Network event callback types
 */
typedef enum {
    NETWORK_EVENT_CONNECTED = 1,
    NETWORK_EVENT_DISCONNECTED = 2,
    NETWORK_EVENT_DATA_RECEIVED = 3,
    NETWORK_EVENT_DATA_SENT = 4,
    NETWORK_EVENT_ERROR = 5,
    NETWORK_EVENT_TIMEOUT = 6
} network_event_t;

/**
 * @brief Network event callback function type
 * @param connection Connection handle
 * @param event Event type
 * @param data Event data
 * @param data_len Data length
 * @param user_data User data
 */
typedef void (*network_event_callback_t)(network_connection_t *connection, network_event_t event,
                                         const void *data, size_t data_len, void *user_data);

/**
 * @brief Create a default network configuration
 * @return Default network configuration structure
 */
network_config_t network_create_default_config(void);

/**
 * @brief Create a network connection
 * @param config Network configuration
 * @return Connection handle, NULL on failure
 * @ownership Caller releases with network_connection_destroy
 */
network_connection_t *network_connection_create(const network_config_t *config);

/**
 * @brief Destroy a network connection
 * @param connection Connection handle
 */
void network_connection_destroy(network_connection_t *connection);

/**
 * @brief Establish a network connection
 * @param connection Connection handle
 * @return Error code
 */
airy_err_t network_connect(network_connection_t *connection);

/**
 * @brief Disconnect a network connection
 * @param connection Connection handle
 * @return Error code
 */
airy_err_t network_disconnect(network_connection_t *connection);

/**
 * @brief Send data
 * @param connection Connection handle
 * @param data Data buffer
 * @param length Data length
 * @param sent [out] Actual bytes sent (optional)
 * @return Error code
 */
airy_err_t network_send(network_connection_t *connection, const void *data, size_t length,
                        size_t *sent);

/**
 * @brief Receive data
 * @param connection Connection handle
 * @param buffer Receive buffer
 * @param length Buffer length
 * @param received [out] Actual bytes received (optional)
 * @return Error code
 */
airy_err_t network_receive(network_connection_t *connection, void *buffer, size_t length,
                           size_t *received);

/**
 * @brief Send all data (loops until complete)
 * @param connection Connection handle
 * @param data Data buffer
 * @param length Data length
 * @return Error code
 */
airy_err_t network_send_all(network_connection_t *connection, const void *data, size_t length);

/**
 * @brief Receive an exact amount of data
 * @param connection Connection handle
 * @param buffer Receive buffer
 * @param length Expected length
 * @return Error code
 */
airy_err_t network_receive_exact(network_connection_t *connection, void *buffer, size_t length);

/**
 * @brief Get the connection status
 * @param connection Connection handle
 * @return Connection status
 */
network_status_t network_get_status(const network_connection_t *connection);

/**
 * @brief Set the connection timeout
 * @param connection Connection handle
 * @param timeout_ms Timeout in milliseconds
 * @return Error code
 */
airy_err_t network_set_timeout(network_connection_t *connection, int timeout_ms);

/**
 * @brief Set read/write timeouts
 * @param connection Connection handle
 * @param read_timeout_ms Read timeout (ms)
 * @param write_timeout_ms Write timeout (ms)
 * @return Error code
 */
airy_err_t network_set_rw_timeout(network_connection_t *connection, int read_timeout_ms,
                                  int write_timeout_ms);

/**
 * @brief Get statistics
 * @param connection Connection handle
 * @param stats [out] Statistics
 * @return Error code
 */
airy_err_t network_get_stats(const network_connection_t *connection, network_stats_t *stats);

/**
 * @brief Reset statistics
 * @param connection Connection handle
 * @return Error code
 */
airy_err_t network_reset_stats(network_connection_t *connection);

/**
 * @brief Set the event callback
 * @param connection Connection handle
 * @param callback Callback function
 * @param user_data User data
 * @return Error code
 */
airy_err_t network_set_event_callback(network_connection_t *connection,
                                      network_event_callback_t callback, void *user_data);

/**
 * @brief Get the error message
 * @param connection Connection handle
 * @return Error message string
 */
const char *network_get_error_message(const network_connection_t *connection);

/**
 * @brief Perform an HTTP request
 * @param connection Connection handle (must be connected)
 * @param request HTTP request configuration
 * @param response [out] HTTP response (caller releases with network_http_response_free)
 * @return Error code
 */
airy_err_t network_http_request(network_connection_t *connection,
                                const network_http_request_t *request,
                                network_http_response_t *response);

/**
 * @brief Perform an HTTP GET request
 * @param connection Connection handle
 * @param path Request path
 * @param response [out] HTTP response
 * @return Error code
 */
airy_err_t network_http_get(network_connection_t *connection, const char *path,
                            network_http_response_t *response);

/**
 * @brief Perform an HTTP POST request
 * @param connection Connection handle
 * @param path Request path
 * @param content_type Content-Type
 * @param body Request body
 * @param body_len Request body length
 * @param response [out] HTTP response
 * @return Error code
 */
airy_err_t network_http_post(network_connection_t *connection, const char *path,
                             const char *content_type, const void *body, size_t body_len,
                             network_http_response_t *response);

/**
 * @brief Free HTTP response resources
 * @param response HTTP response structure
 */
void network_http_response_free(network_http_response_t *response);

/**
 * @brief Create a connection pool
 * @param config Base network configuration
 * @param pool_size Pool size
 * @return Connection pool handle, NULL on failure
 */
network_pool_t *network_pool_create(const network_config_t *config, size_t pool_size);

/**
 * @brief Destroy a connection pool
 * @param pool Connection pool handle
 */
void network_pool_destroy(network_pool_t *pool);

/**
 * @brief Acquire a connection from the pool
 * @param pool Connection pool handle
 * @param timeout_ms Timeout in milliseconds
 * @return Connection handle, NULL on failure
 * @note The acquired connection must be released with
 *       network_pool_release_connection after use
 */
network_connection_t *network_pool_acquire(network_pool_t *pool, int timeout_ms);

/**
 * @brief Release a connection back to the pool
 * @param pool Connection pool handle
 * @param connection Connection handle
 */
void network_pool_release(network_pool_t *pool, network_connection_t *connection);

/**
 * @brief Get the number of available connections in the pool
 * @param pool Connection pool handle
 * @return Number of available connections
 */
size_t network_pool_available(const network_pool_t *pool);

/**
 * @brief Get the total pool size
 * @param pool Connection pool handle
 * @return Total size
 */
size_t network_pool_size(const network_pool_t *pool);

/**
 * @brief Health-check the connection pool
 * @param pool Connection pool handle
 * @return Number of healthy connections
 */
size_t network_pool_health_check(network_pool_t *pool);

/**
 * @brief DNS resolution result structure
 */
typedef struct {
    char **addresses;
    size_t count;
    int *ports;
} network_dns_result_t;

/**
 * @brief Perform DNS resolution
 * @param hostname Hostname
 * @param af Address family
 * @param result [out] Resolution result
 * @return Error code
 */
airy_err_t network_dns_resolve(const char *hostname, network_af_t af, network_dns_result_t *result);

/**
 * @brief Free DNS resolution results
 * @param result Resolution result
 */
void network_dns_result_free(network_dns_result_t *result);

/**
 * @brief Check whether a host is reachable
 * @param host Hostname or IP
 * @param timeout_ms Timeout in milliseconds
 * @return true if reachable, false otherwise
 */
bool network_is_reachable(const char *host, int timeout_ms);

/**
 * @brief Get the local IP address
 * @param af Address family
 * @param buffer Output buffer
 * @param buffer_len Buffer length
 * @return Error code
 */
airy_err_t network_get_local_ip(network_af_t af, char *buffer, size_t buffer_len);

/**
 * @brief Convert an IP address to a string
 * @param af Address family
 * @param addr Address structure
 * @param buffer Output buffer
 * @param buffer_len Buffer length
 * @return Error code
 */
airy_err_t network_addr_to_string(network_af_t af, const void *addr, char *buffer,
                                  size_t buffer_len);

/**
 * @brief Initialize the network subsystem
 * @return Error code
 * @note On Windows this function must be called to initialize Winsock
 */
airy_err_t network_init(void);

/**
 * @brief Clean up the network subsystem
 * @note On Windows this function must be called to clean up Winsock
 */
void network_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_NETWORK_COMMON_H */
