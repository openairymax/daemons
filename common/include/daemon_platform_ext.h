// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file daemon_platform_ext.h
 * @brief Daemon 模块平台扩展声明
 *
 * P0.17 阶段 2：将 daemons 特有的平台函数声明和类型从 daemons 版
 * platform.h 中独立出来。由于 -I 路径顺序中 commons/platform/include
 * 排在 daemons/common/include 之前，daemons 子模块源文件的
 * #include "platform.h" 总是找到 commons 版 platform.h（无 daemons 特有
 * 函数声明，如 agentrt_dl_*、agentrt_socket_create_tcp_server 等）。
 *
 * 本文件解决此问题：daemons 子模块源文件通过
 * #include "daemon_platform_ext.h" 获取 daemons 特有声明，不依赖
 * platform.h 的同名覆盖机制。
 *
 * 设计原则：
 * - 不重复 commons 版 platform.h 已有的声明（agentrt_sleep_ms、
 *   agentrt_get_sysinfo、AGENTRT_THREAD_LOCAL 等）
 * - 只包含 daemons 特有的函数声明、类型和兼容别名
 * - 函数实现在 daemons/common/src/platform_compat.c
 *
 * @see agentrt/commons/platform/include/platform.h  (commons 权威平台抽象)
 * @see agentrt/daemons/common/src/platform_compat.c (daemons 特有实现)
 */

#ifndef AGENTRT_DAEMON_PLATFORM_EXT_H
#define AGENTRT_DAEMON_PLATFORM_EXT_H

/* 确保 commons 版 platform.h 已被包含（提供 agentrt_mutex_t、agentrt_cond_t、
 * agentrt_thread_t、agentrt_socket_t、AGENTRT_PLATFORM_POSIX 等基础类型和宏） */
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 兼容别名 ==================== */
/* 旧代码可能使用 agentrt_platform_* 前缀，此处提供兼容别名 */

typedef agentrt_mutex_t agentrt_platform_mutex_t;
#define agentrt_platform_mutex_init agentrt_mutex_init
#define agentrt_platform_mutex_lock agentrt_mutex_lock
#define agentrt_platform_mutex_unlock agentrt_mutex_unlock
#define agentrt_platform_mutex_destroy agentrt_mutex_destroy
#define agentrt_platform_get_time_ms agentrt_time_ms

typedef agentrt_cond_t agentrt_platform_cond_val_t;
#define agentrt_platform_cond_init agentrt_cond_init
#define agentrt_platform_cond_destroy agentrt_cond_destroy
#define agentrt_platform_cond_wait agentrt_cond_wait
#define agentrt_platform_cond_timedwait agentrt_cond_timedwait
#define agentrt_platform_cond_signal agentrt_cond_signal
#define agentrt_platform_cond_broadcast agentrt_cond_broadcast

typedef agentrt_thread_t agentrt_platform_thread_t;
/* agentrt_platform_thread_create/join/detach 已由 commons 版 platform.h 提供 */

/* ==================== Handle 类型兼容 ==================== */

typedef agentrt_mutex_t *agentrt_mutex_handle_t;
typedef agentrt_cond_t *agentrt_cond_handle_t;
typedef agentrt_cond_t *agentrt_platform_cond_t;
typedef agentrt_thread_t *agentrt_thread_handle_t;
typedef agentrt_socket_t *agentrt_socket_handle_t;

/* ==================== 额外类型定义 ==================== */

#ifndef AGENTRT_TIMESTAMP_T_DEFINED
#define AGENTRT_TIMESTAMP_T_DEFINED
typedef uint64_t agentrt_timestamp_t;
#endif

typedef struct {
    uint64_t seconds;
    uint32_t nanoseconds;
} agentrt_timestamp_struct_t;

typedef struct {
    int exit_code;
    int signal;
} agentrt_process_status_t;

/* ==================== Socket 额外类型 ==================== */

typedef enum { AGENTRT_AF_INET, AGENTRT_AF_INET6, AGENTRT_AF_UNIX } agentrt_address_family_t;

typedef enum {
    AGENTRT_SOCK_STREAM,
    AGENTRT_SOCK_DGRAM,
    AGENTRT_SOCK_SEQPACKET
} agentrt_socket_type_t;

typedef struct {
    agentrt_address_family_t family;
    uint16_t port;
    union {
        uint8_t ipv4[4];
        uint8_t ipv6[16];
        char path[108];
    } addr;
} agentrt_sockaddr_t;

/* ==================== Daemon 特有函数声明 ==================== */
/* 实现在 daemons/common/src/platform_compat.c */

int agentrt_time_now(agentrt_timestamp_t *ts);
int agentrt_time_monotonic(agentrt_timestamp_t *ts);
uint64_t agentrt_time_to_ms(const agentrt_timestamp_t *ts);
void agentrt_time_from_ms(uint64_t ms, agentrt_timestamp_t *ts);
/* agentrt_sleep_ms 已由 commons 版 platform.h 提供，此处不重复 */
uint32_t agentrt_process_self(void);
uint64_t agentrt_thread_self(void);
int agentrt_thread_setname(const char *name);
int agentrt_thread_getname(char *name, size_t size);
int agentrt_mkdir(const char *path, int recursive);

/* ==================== 动态库加载 (DL) 抽象 ==================== */

#ifndef AGENTRT_DL_T_DEFINED
#define AGENTRT_DL_T_DEFINED
typedef void *agentrt_dl_t;
#endif

agentrt_dl_t agentrt_dl_open(const char *path);
int agentrt_dl_close(agentrt_dl_t dl);
void *agentrt_dl_sym(agentrt_dl_t dl, const char *name);
const char *agentrt_dl_error(void);

/* ==================== 服务器端 Socket 兼容 ==================== */

int agentrt_socket_init(void);
void agentrt_socket_cleanup(void);
agentrt_socket_t agentrt_socket_create_tcp_server(const char *host, uint16_t port);

#if AGENTRT_PLATFORM_POSIX
agentrt_socket_t agentrt_socket_create_unix_server(const char *path);
#endif

agentrt_socket_t agentrt_socket_accept(agentrt_socket_t server_fd, uint32_t timeout_ms);
ssize_t agentrt_socket_recv(agentrt_socket_t sock, void *buf, size_t len);
ssize_t agentrt_socket_send(agentrt_socket_t sock, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AGENTRT_DAEMON_PLATFORM_EXT_H */
