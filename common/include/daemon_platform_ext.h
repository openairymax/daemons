/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_platform_ext.h
 * @brief Daemon-module platform extension declarations.
 *
 * P0.17 phase 2: split daemon-specific platform declarations and types out
 * of the daemons platform.h. Due to -I path ordering (commons/platform/include
 * precedes daemons/common/include), #include "platform.h" in daemon sources
 * always resolves to the commons version (which lacks daemon-specific
 * declarations such as airy_dl_*, airy_sock_create_tcp_server).
 *
 * This file solves that: daemon sources include "daemon_platform_ext.h" to
 * get daemon-specific declarations, without relying on platform.h name
 * overriding.
 *
 * Design principles:
 * - No duplication of commons platform.h declarations (airy_sleep_ms,
 *   airy_get_sysinfo, AIRY_THREAD_LOCAL, etc.)
 * - Only daemon-specific function declarations, types and compat aliases
 * - Implementations live in daemons/common/src/platform_compat.c
 *
 * @see agentrt/commons/platform/include/platform.h  (commons authoritative)
 * @see agentrt/daemons/common/src/platform_compat.c (daemon-specific impl)
 */

#ifndef AIRY_RT_DAEMON_PLATFORM_EXT_H
#define AIRY_RT_DAEMON_PLATFORM_EXT_H

/* Ensure the commons platform.h is included (provides airy_mtx_t,
 * airy_cond_t, airy_thread_t, airy_sock_t, AIRY_PLATFORM_POSIX, ...) */
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef airy_mtx_t airy_platform_mutex_t;
#define airy_platform_mutex_init airy_mtx_init
#define airy_platform_mutex_lock airy_mtx_lock
#define airy_platform_mutex_unlock airy_mtx_unlock
#define airy_platform_mutex_destroy airy_mtx_destroy
#define airy_platform_get_time_ms airy_time_ms

typedef airy_cond_t airy_platform_cond_val_t;
#define airy_platform_cond_init airy_cond_init
#define airy_platform_cond_destroy airy_cond_destroy
#define airy_platform_cond_wait airy_cond_wait
#define airy_platform_cond_timedwait airy_cond_timedwait
#define airy_platform_cond_signal airy_cond_signal
#define airy_platform_cond_broadcast airy_cond_broadcast

typedef airy_thread_t airy_platform_thread_t;


typedef airy_mtx_t *airy_mtx_handle_t;
typedef airy_cond_t *airy_cond_handle_t;
typedef airy_cond_t *airy_platform_cond_t;
typedef airy_thread_t *airy_thread_handle_t;
typedef airy_sock_t *airy_sock_handle_t;


#ifndef AIRY_TIMESTAMP_T_DEFINED
#define AIRY_TIMESTAMP_T_DEFINED
typedef uint64_t airy_timestamp_t;
#endif

typedef struct {
    uint64_t seconds;
    uint32_t nanoseconds;
} airy_timestamp_struct_t;

typedef struct {
    int exit_code;
    int signal;
} airy_process_status_t;


typedef enum { AIRY_AF_INET, AIRY_AF_INET6, AIRY_AF_UNIX } airy_address_family_t;

typedef enum { AIRY_SOCK_STREAM, AIRY_SOCK_DGRAM, AIRY_SOCK_SEQPACKET } airy_sock_type_t;

typedef struct {
    airy_address_family_t family;
    uint16_t port;
    union {
        uint8_t ipv4[4];
        uint8_t ipv6[16];
        char path[108];
    } addr;
} airy_sockaddr_t;


int airy_time_now(airy_timestamp_t *ts);
int airy_time_monotonic(airy_timestamp_t *ts);
uint64_t airy_time_to_ms(const airy_timestamp_t *ts);
void airy_time_from_ms(uint64_t ms, airy_timestamp_t *ts);

uint32_t airy_process_self(void);
uint64_t airy_thread_self(void);
int airy_thread_setname(const char *name);
int airy_thread_getname(char *name, size_t size);
int airy_mkdir(const char *path, int recursive);


#ifndef AIRY_DL_T_DEFINED
#define AIRY_DL_T_DEFINED
typedef void *airy_dl_t;
#endif

airy_dl_t airy_dl_open(const char *path);
int airy_dl_close(airy_dl_t dl);
void *airy_dl_sym(airy_dl_t dl, const char *name);
const char *airy_dl_error(void);


int airy_sock_init(void);
void airy_sock_cleanup(void);
airy_sock_t airy_sock_create_tcp_server(const char *host, uint16_t port);

#if AIRY_PLATFORM_POSIX
airy_sock_t airy_sock_create_unix_server(const char *path);
#endif

airy_sock_t airy_sock_accept(airy_sock_t server_fd, uint32_t timeout_ms);
ssize_t airy_sock_recv(airy_sock_t sock, void *buf, size_t len);
ssize_t airy_sock_send(airy_sock_t sock, const void *buf, size_t len);

/* Read a complete JSON-RPC request frame from a daemon client socket.
 * Loop poll+recv probing JSON completeness (mirrors the client-side
 * rpc_recv_response), lifting the historical single-recv 64 KiB cap so
 * multi-round tool-loop requests (e.g. llm.complete feeding back large
 * web_fetch results) are no longer truncated. Returns an AIRY_MALLOC buffer
 * (caller frees) with *out_len set; NULL + *err (static string) on failure. */
char *airy_daemon_read_request(airy_sock_t client_fd, size_t *out_len, const char **err);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_PLATFORM_EXT_H */
