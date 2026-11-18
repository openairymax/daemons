// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file network_dns.c
 * @brief Network module - DNS and address resolution domain.
 *
 * Implements the DNS resolution, reachability probing, local IP query
 * and address-to-string functions declared in network_common.h.
 * Following ARCHITECTURAL_PRINCIPLES.md design principles:
 * - E-4 Cross-platform consistency: Windows/Linux/macOS
 * - E-5 Semantic naming: every function name states its purpose
 * - E-6 Traceable errors: unified error code system
 */

#include "network_common_internal.h"

airy_err_t network_dns_resolve(const char *hostname, network_af_t af, network_dns_result_t *result)
{
    if (!hostname || !result) {
        return AIRY_EINVAL;
    }

    network_init_winsock();

    struct addrinfo hints, *res;
    AIRY_MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = af_to_native(af);
    hints.ai_socktype = SOCK_STREAM;

    int gai_ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (gai_ret != 0) {
        return AIRY_EIO;
    }

    int count = 0;
    struct addrinfo *p = res;
    while (p) {
        count++;
        p = p->ai_next;
    }

    if (count == 0) {
        freeaddrinfo(res);
        return AIRY_ENOENT;
    }

    result->addresses = (char **)AIRY_CALLOC((size_t)count, sizeof(char *));
    result->ports = (int *)AIRY_CALLOC((size_t)count, sizeof(int));
    result->count = (size_t)count;

    if (!result->addresses || !result->ports) {
        AIRY_FREE(result->addresses);
        AIRY_FREE(result->ports);
        freeaddrinfo(res);
        return AIRY_ENOMEM;
    }

    p = res;
    for (int i = 0; i < count && p; i++) {
        char ip_str[INET6_ADDRSTRLEN];

        if (p->ai_family == AF_INET) {
            struct sockaddr_in *addr_in = (struct sockaddr_in *)p->ai_addr;
            inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
            result->ports[i] = ntohs(addr_in->sin_port);
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)p->ai_addr;
            inet_ntop(AF_INET6, &addr_in6->sin6_addr, ip_str, sizeof(ip_str));
            result->ports[i] = ntohs(addr_in6->sin6_port);
        } else {
            AIRY_STRNCPY_TERM(ip_str, "unknown", INET6_ADDRSTRLEN);
            ip_str[INET6_ADDRSTRLEN - 1] = '\0';
            result->ports[i] = 0;
        }

        result->addresses[i] = AIRY_STRDUP(ip_str);
        p = p->ai_next;
    }

    freeaddrinfo(res);
    return AIRY_SUCCESS;
}

void network_dns_result_free(network_dns_result_t *result)
{
    if (!result) {
        return;
    }

    if (result->addresses) {
        for (size_t i = 0; i < result->count; i++) {
            if (result->addresses[i]) {
                AIRY_FREE(result->addresses[i]);
            }
        }
        AIRY_FREE(result->addresses);
        result->addresses = NULL;
    }

    if (result->ports) {
        AIRY_FREE(result->ports);
        result->ports = NULL;
    }

    result->count = 0;
}

bool network_is_reachable(const char *host, int timeout_ms)
{
    if (!host) {
        return false;
    }

    network_init_winsock();

    network_config_t config = network_create_default_config();
    config.host = host;
    config.timeout_ms = timeout_ms > 0 ? timeout_ms : 5000;

    network_connection_t *conn = network_connection_create(&config);
    if (!conn) {
        return false;
    }

    airy_err_t err = network_connect(conn);
    bool reachable = (err == AIRY_SUCCESS);

    if (reachable) {
        network_disconnect(conn);
    }

    network_connection_destroy(conn);

    return reachable;
}

airy_err_t network_get_local_ip(network_af_t af, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return AIRY_EINVAL;
    }

    network_init_winsock();

#ifdef _WIN32
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    struct addrinfo hints, *res;
    AIRY_MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = af_to_native(af);

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        AIRY_STRNCPY_TERM(buffer, "127.0.0.1", buffer_len);
        buffer[buffer_len - 1] = '\0';
        return AIRY_SUCCESS;
    }

    if (res->ai_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &addr_in->sin_addr, buffer, (socklen_t)buffer_len);
    } else if (res->ai_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)res->ai_addr;
        inet_ntop(AF_INET6, &addr_in6->sin6_addr, buffer, (socklen_t)buffer_len);
    } else {
        AIRY_STRNCPY_TERM(buffer, "127.0.0.1", buffer_len);
        buffer[buffer_len - 1] = '\0';
    }

    freeaddrinfo(res);
#else

    const char *test_host = "8.8.8.8";
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        AIRY_STRNCPY_TERM(buffer, "127.0.0.1", buffer_len);
        buffer[buffer_len - 1] = '\0';
        return AIRY_SUCCESS;
    }

    struct sockaddr_in server;
    AIRY_MEMSET(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(80);
    inet_pton(AF_INET, test_host, &server.sin_addr);

    connect(sockfd, (struct sockaddr *)&server, sizeof(server));

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len);
    inet_ntop(AF_INET, &local_addr.sin_addr, buffer, (socklen_t)buffer_len);

    close(sockfd);
#endif

    return AIRY_SUCCESS;
}

airy_err_t network_addr_to_string(network_af_t af, const void *addr, char *buffer,
                                  size_t buffer_len)
{
    if (!addr || !buffer || buffer_len == 0) {
        return AIRY_EINVAL;
    }

    if (af == NETWORK_AF_INET) {
        const struct sockaddr_in *addr_in = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &addr_in->sin_addr, buffer, (socklen_t)buffer_len);
    } else if (af == NETWORK_AF_INET6) {
        const struct sockaddr_in6 *addr_in6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &addr_in6->sin6_addr, buffer, (socklen_t)buffer_len);
    } else {
        AIRY_STRNCPY_TERM(buffer, "unknown", buffer_len);
        buffer[buffer_len - 1] = '\0';
    }

    return AIRY_SUCCESS;
}
