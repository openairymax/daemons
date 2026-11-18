// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file network_common_internal.h
 * @brief Network module internal shared definitions: connection/connection
 * pool structs and cross-file helper declarations.
 *
 * Also serves as the family prelude: the shared socket/system headers and
 * the platform shims (Winsock init, strdup mapping) live here so each
 * split unit (common/dns/http/pool) keeps only its own doc block plus this
 * single include.
 */

#ifndef AIRY_NETWORK_COMMON_INTERNAL_H
#define AIRY_NETWORK_COMMON_INTERNAL_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "airy_memory.h"
#include "network_common.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _CRT_NONSTDC_NO_DEPRECATE
#ifdef _WIN32
#define strdup _strdup
#endif
#include <stdarg.h>
#include "atomic_compat.h"

#include "error.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

struct network_connection {
    network_config_t config;
    network_status_t status;
    char error_msg[256];
    network_event_callback_t event_cb;
    void *event_user_data;
    network_stats_t stats;
#ifdef _WIN32
    SOCKET sock;
#else
    int fd;
#endif
    struct sockaddr_in addr;
};

struct network_pool {
    network_config_t base_config;
    size_t max_size;
    size_t current_size;
    struct network_connection **connections;
};

int network_init_winsock(void);

void set_nonblocking_mode(void *handle);

void set_socket_timeout(void *handle, int timeout_ms, int is_recv);

int af_to_native(network_af_t af);

int socktype_to_native(network_sock_type_t st);

#endif /* AIRY_NETWORK_COMMON_INTERNAL_H */
