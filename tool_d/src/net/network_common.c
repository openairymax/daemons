// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file network_common.c
 * @brief Network module implementation - connection lifecycle and IO domain.
 *
 * Implements the connection lifecycle and send/receive IO functions
 * declared in network_common.h, and keeps module-level internal helpers
 * (Winsock init, timeout/non-blocking setup, address family conversion).
 * Following ARCHITECTURAL_PRINCIPLES.md design principles:
 * - E-4 Cross-platform consistency: Windows/Linux/macOS
 * - E-5 Semantic naming: every function name states its purpose
 * - E-6 Traceable errors: unified error code system
 */

#include "network_common_internal.h"

int network_init_winsock(void)
{
#ifdef _WIN32
    static atomic_int initialized = 0;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&initialized, &expected, 1, memory_order_acq_rel,
                                                memory_order_relaxed)) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            atomic_store_explicit(&initialized, 0, memory_order_release);
            return AIRY_EINVAL;
        }
    }
#endif
    return 0;
}

void set_nonblocking_mode(void *handle)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket((SOCKET)(uintptr_t)handle, FIONBIO, &mode);
#else
    int flags = fcntl((int)(intptr_t)handle, F_GETFL, 0);
    fcntl((int)(intptr_t)handle, F_SETFL, flags | O_NONBLOCK);
#endif
}

void set_socket_timeout(void *handle, int timeout_ms, int is_recv)
{
#ifdef _WIN32
    DWORD ms = (DWORD)timeout_ms;
    if (is_recv) {
        setsockopt((SOCKET)(uintptr_t)handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms,
                   sizeof(ms));
    } else {
        setsockopt((SOCKET)(uintptr_t)handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms,
                   sizeof(ms));
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (is_recv) {
        setsockopt((int)(intptr_t)handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    } else {
        setsockopt((int)(intptr_t)handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif
}

int af_to_native(network_af_t af)
{
    switch (af) {
    case NETWORK_AF_INET:
        return AF_INET;
    case NETWORK_AF_INET6:
        return AF_INET6;
    default:
        return AF_INET;
    }
}

int socktype_to_native(network_sock_type_t st)
{
    switch (st) {
    case NETWORK_SOCK_STREAM:
        return SOCK_STREAM;
    case NETWORK_SOCK_DGRAM:
        return SOCK_DGRAM;
    case NETWORK_SOCK_RAW:
        return SOCK_RAW;
    default:
        return SOCK_STREAM;
    }
}


network_config_t network_create_default_config(void)
{
    network_config_t config = {0};

    config.host = "127.0.0.1";
    config.port = 8080;
    config.timeout_ms = 30000;
    config.read_timeout_ms = 10000;
    config.write_timeout_ms = 10000;
    config.max_retries = 3;
    config.retry_interval_ms = 1000;
    config.sock_type = NETWORK_SOCK_STREAM;
    config.af = NETWORK_AF_INET;
    config.keepalive = false;
    config.nonblocking = false;
    config.ssl_enable = false;
    config.ssl_verify = NETWORK_SSL_VERIFY_PEER;
    config.ssl_cert_path = NULL;
    config.ssl_key_path = NULL;
    config.ssl_ca_path = NULL;

    return config;
}

network_connection_t *network_connection_create(const network_config_t *config)
{
    if (!config) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    network_connection_t *conn =
        (network_connection_t *)AIRY_CALLOC(1, sizeof(network_connection_t));
    if (!conn) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    conn->config = *config;
    conn->status = NETWORK_STATUS_DISCONNECTED;
    AIRY_MEMSET(conn->error_msg, 0, sizeof(conn->error_msg));
    conn->event_cb = NULL;
    conn->event_user_data = NULL;
    AIRY_MEMSET(&conn->stats, 0, sizeof(network_stats_t));

#ifdef _WIN32
    conn->sock = INVALID_SOCKET;
#else
    conn->fd = -1;
#endif

    AIRY_MEMSET(&conn->addr, 0, sizeof(conn->addr));

    return conn;
}

void network_connection_destroy(network_connection_t *connection)
{
    if (!connection) {
        return;
    }

    if (connection->status == NETWORK_STATUS_CONNECTED ||
        connection->status == NETWORK_STATUS_CONNECTING) {
        network_disconnect(connection);
    }

    AIRY_FREE(connection);
}

airy_err_t network_connect(network_connection_t *connection)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    network_init_winsock();

    if (connection->status != NETWORK_STATUS_DISCONNECTED &&
        connection->status != NETWORK_STATUS_ERROR) {
        snprintf(connection->error_msg, sizeof(connection->error_msg),
                 "Connection already in progress or connected");
        return AIRY_EBUSY;
    }

    connection->status = NETWORK_STATUS_CONNECTING;

    int native_af = af_to_native(connection->config.af);
    int native_type = socktype_to_native(connection->config.sock_type);

#ifdef _WIN32
    SOCKET s = socket(native_af, native_type, 0);
    if (s == INVALID_SOCKET) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "socket() failed: %d",
                 WSAGetLastError());
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_EIO;
    }
    connection->sock = s;
#else
    int fd = socket(native_af, native_type, 0);
    if (fd < 0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "socket() failed: %s",
                 strerror(errno));
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_EIO;
    }
    connection->fd = fd;
#endif

    AIRY_MEMSET(&connection->addr, 0, sizeof(connection->addr));
    connection->addr.sin_family = AF_INET;
    connection->addr.sin_port = htons((uint16_t)connection->config.port);

    if (inet_pton(AF_INET, connection->config.host, &connection->addr.sin_addr) <= 0) {
        struct addrinfo hints, *result;
        AIRY_MEMSET(&hints, 0, sizeof(hints));
        hints.ai_family = native_af;
        hints.ai_socktype = native_type;

        int gai_ret = getaddrinfo(connection->config.host, NULL, &hints, &result);
        if (gai_ret != 0) {
            snprintf(connection->error_msg, sizeof(connection->error_msg),
                     "DNS resolution failed for %s: %s", connection->config.host,
                     gai_strerror(gai_ret));
            network_disconnect(connection);
            connection->status = NETWORK_STATUS_ERROR;
            return AIRY_EIO;
        }

        struct sockaddr_in *addr_in = (struct sockaddr_in *)result->ai_addr;
        connection->addr.sin_addr = addr_in->sin_addr;
        freeaddrinfo(result);
    }

#ifdef _WIN32
    void *handle = (void *)(uintptr_t)connection->sock;
#else
    void *handle = (void *)(intptr_t)connection->fd;
#endif
    set_socket_timeout(handle, connection->config.timeout_ms, 1);
    set_socket_timeout(handle, connection->config.timeout_ms, 0);

    if (connection->config.nonblocking) {
        set_nonblocking_mode(handle);
    }

#ifdef _WIN32
    if (connect(connection->sock, (struct sockaddr *)&connection->addr, sizeof(connection->addr)) ==
        SOCKET_ERROR) {
        int err = WSAGetLastError();
        snprintf(connection->error_msg, sizeof(connection->error_msg), "connect() failed: %d", err);
        network_disconnect(connection);
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_EIO;
    }
#else
    if (connect(connection->fd, (struct sockaddr *)&connection->addr, sizeof(connection->addr)) <
        0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "connect() failed: %s",
                 strerror(errno));
        network_disconnect(connection);
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_EIO;
    }
#endif

    connection->status = NETWORK_STATUS_CONNECTED;
    connection->stats.connect_count++;

    if (connection->event_cb) {
        connection->event_cb(connection, NETWORK_EVENT_CONNECTED, NULL, 0,
                             connection->event_user_data);
    }

    return AIRY_SUCCESS;
}

airy_err_t network_disconnect(network_connection_t *connection)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    if (connection->status != NETWORK_STATUS_CONNECTED &&
        connection->status != NETWORK_STATUS_CONNECTING &&
        connection->status != NETWORK_STATUS_ERROR) {
        return AIRY_SUCCESS;
    }

    connection->status = NETWORK_STATUS_DISCONNECTING;

    if (connection->event_cb) {
        connection->event_cb(connection, NETWORK_EVENT_DISCONNECTED, NULL, 0,
                             connection->event_user_data);
    }

#ifdef _WIN32
    if (connection->sock != INVALID_SOCKET) {
        closesocket(connection->sock);
        connection->sock = INVALID_SOCKET;
    }
#else
    if (connection->fd >= 0) {
        close(connection->fd);
        connection->fd = -1;
    }
#endif

    connection->status = NETWORK_STATUS_DISCONNECTED;

    return AIRY_SUCCESS;
}

airy_err_t network_send(network_connection_t *connection, const void *data, size_t length,
                        size_t *sent)
{
    if (!connection || !data || length == 0) {
        return AIRY_EINVAL;
    }

    if (connection->status != NETWORK_STATUS_CONNECTED) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "Not connected");
        return AIRY_ENOTCONN;
    }

#ifdef _WIN32
    int result = send(connection->sock, (const char *)data, (int)length, 0);
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        snprintf(connection->error_msg, sizeof(connection->error_msg), "send() failed: %d", err);
        connection->stats.error_count++;
        return AIRY_EIO;
    }
#else
    ssize_t result = write(connection->fd, data, length);
    if (result < 0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "write() failed: %s",
                 strerror(errno));
        connection->stats.error_count++;
        return AIRY_EIO;
    }
#endif

    if (sent) {
        *sent = (size_t)result;
    }

    connection->stats.bytes_sent += result;
    connection->stats.packets_sent++;

    if (connection->event_cb && result > 0) {
        connection->event_cb(connection, NETWORK_EVENT_DATA_SENT, data, result,
                             connection->event_user_data);
    }

    return AIRY_SUCCESS;
}

airy_err_t network_receive(network_connection_t *connection, void *buffer, size_t length,
                           size_t *received)
{
    if (!connection || !buffer || length == 0) {
        return AIRY_EINVAL;
    }

    if (connection->status != NETWORK_STATUS_CONNECTED) {
        return AIRY_ENOTCONN;
    }

#ifdef _WIN32
    int result = recv(connection->sock, (char *)buffer, (int)length, 0);
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        snprintf(connection->error_msg, sizeof(connection->error_msg), "recv() failed: %d", err);
        connection->stats.error_count++;
        return AIRY_EIO;
    }
    if (result == 0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "Connection closed");
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_ECONNRESET;
    }
#else
    ssize_t result = read(connection->fd, buffer, length);
    if (result < 0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "read() failed: %s",
                 strerror(errno));
        connection->stats.error_count++;
        return AIRY_EIO;
    }
    if (result == 0) {
        snprintf(connection->error_msg, sizeof(connection->error_msg), "Connection closed");
        connection->status = NETWORK_STATUS_ERROR;
        return AIRY_ECONNRESET;
    }
#endif

    if (received) {
        *received = (size_t)result;
    }

    connection->stats.bytes_received += result;
    connection->stats.packets_received++;

    if (connection->event_cb && result > 0) {
        connection->event_cb(connection, NETWORK_EVENT_DATA_RECEIVED, buffer, result,
                             connection->event_user_data);
    }

    return AIRY_SUCCESS;
}

airy_err_t network_send_all(network_connection_t *connection, const void *data, size_t length)
{
    if (!connection || !data || length == 0) {
        return AIRY_EINVAL;
    }

    const uint8_t *ptr = (const uint8_t *)data;
    size_t remaining = length;
    int retries = 0;
    int max_retries = connection->config.max_retries > 0 ? connection->config.max_retries :
                                                           NETWORK_DEFAULT_MAX_RETRIES;

    while (remaining > 0) {
        size_t sent = 0;
        airy_err_t err = network_send(connection, ptr, remaining, &sent);
        if (err != AIRY_SUCCESS) {
            retries++;
            if (retries >= max_retries) {
                connection->stats.retry_count++;
                return err;
            }
            continue;
        }

        ptr += sent;
        remaining -= sent;
        retries = 0;
    }

    return AIRY_SUCCESS;
}

airy_err_t network_receive_exact(network_connection_t *connection, void *buffer, size_t length)
{
    if (!connection || !buffer || length == 0) {
        return AIRY_EINVAL;
    }

    uint8_t *ptr = (uint8_t *)buffer;
    size_t remaining = length;
    int retries = 0;
    int max_retries = connection->config.max_retries > 0 ? connection->config.max_retries :
                                                           NETWORK_DEFAULT_MAX_RETRIES;

    while (remaining > 0) {
        size_t received = 0;
        airy_err_t err = network_receive(connection, ptr, remaining, &received);
        if (err != AIRY_SUCCESS) {
            retries++;
            if (retries >= max_retries) {
                connection->stats.retry_count++;
                return err;
            }
            continue;
        }

        if (received == 0) {
            break;
        }

        ptr += received;
        remaining -= received;
        retries = 0;
    }

    if (remaining > 0) {
        return AIRY_ETIMEDOUT;
    }

    return AIRY_SUCCESS;
}

network_status_t network_get_status(const network_connection_t *connection)
{
    if (!connection) {
        return NETWORK_STATUS_ERROR;
    }
    return connection->status;
}

airy_err_t network_set_timeout(network_connection_t *connection, int timeout_ms)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    connection->config.timeout_ms = timeout_ms;

    if (connection->status == NETWORK_STATUS_CONNECTED) {
#ifdef _WIN32
        void *handle = (void *)(uintptr_t)connection->sock;
#else
        void *handle = (void *)(intptr_t)connection->fd;
#endif
        set_socket_timeout(handle, timeout_ms, 0);
        set_socket_timeout(handle, timeout_ms, 1);
    }

    return AIRY_SUCCESS;
}

airy_err_t network_set_rw_timeout(network_connection_t *connection, int read_timeout_ms,
                                  int write_timeout_ms)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    connection->config.read_timeout_ms = read_timeout_ms;
    connection->config.write_timeout_ms = write_timeout_ms;

    if (connection->status == NETWORK_STATUS_CONNECTED) {
#ifdef _WIN32
        void *handle = (void *)(uintptr_t)connection->sock;
#else
        void *handle = (void *)(intptr_t)connection->fd;
#endif
        set_socket_timeout(handle, read_timeout_ms, 1);
        set_socket_timeout(handle, write_timeout_ms, 0);
    }

    return AIRY_SUCCESS;
}

airy_err_t network_get_stats(const network_connection_t *connection, network_stats_t *stats)
{
    if (!connection || !stats) {
        return AIRY_EINVAL;
    }

    *stats = connection->stats;
    return AIRY_SUCCESS;
}

airy_err_t network_reset_stats(network_connection_t *connection)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    AIRY_MEMSET(&connection->stats, 0, sizeof(network_stats_t));
    return AIRY_SUCCESS;
}

airy_err_t network_set_event_callback(network_connection_t *connection,
                                      network_event_callback_t callback, void *user_data)
{
    if (!connection) {
        return AIRY_EINVAL;
    }

    connection->event_cb = callback;
    connection->event_user_data = user_data;

    return AIRY_SUCCESS;
}

const char *network_get_error_message(const network_connection_t *connection)
{
    if (!connection) {
        return "Invalid connection handle";
    }
    return connection->error_msg[0] ? connection->error_msg : "No error";
}

airy_err_t network_init(void)
{
    if (network_init_winsock() != 0) {
        return AIRY_EIO;
    }
    return AIRY_SUCCESS;
}

void network_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}
