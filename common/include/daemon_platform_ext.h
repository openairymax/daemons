// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file daemon_platform_ext.h
 * @brief Daemon 模块平台扩展声明
 *
 * P0.17 阶段 2：将 daemons 特有的平台函数声明和类型从 daemons 版
 * platform.h 中独立出来。由于 -I 路径顺序中 commons/platform/include
 * 排在 daemons/common/include 之前，daemons 子模块源文件的
 * #include "platform.h" 总是找到 commons 版 platform.h（无 daemons 特有
 * 函数声明，如 airy_dl_*、airy_sock_create_tcp_server 等）。
 *
 * 本文件解决此问题：daemons 子模块源文件通过
 * #include "daemon_platform_ext.h" 获取 daemons 特有声明，不依赖
 * platform.h 的同名覆盖机制。
 *
 * 设计原则：
 * - 不重复 commons 版 platform.h 已有的声明（airy_sleep_ms、
 *   airy_get_sysinfo、AIRY_THREAD_LOCAL 等）
 * - 只包含 daemons 特有的函数声明、类型和兼容别名
 * - 函数实现在 daemons/common/src/platform_compat.c
 *
 * @see agentrt/commons/platform/include/platform.h  (commons 权威平台抽象)
 * @see agentrt/daemons/common/src/platform_compat.c (daemons 特有实现)
 */

#ifndef AIRY_RT_DAEMON_PLATFORM_EXT_H
#define AIRY_RT_DAEMON_PLATFORM_EXT_H

/* 确保 commons 版 platform.h 已被包含（提供 airy_mtx_t、airy_cond_t、
 * airy_thread_t、airy_sock_t、AIRY_PLATFORM_POSIX 等基础类型和宏） */
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 兼容别名 ==================== */
/* 旧代码可能使用 airy_platform_* 前缀，此处提供兼容别名 */

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
/* airy_platform_thread_create/join/detach 已由 commons 版 platform.h 提供 */

/* ==================== Handle 类型兼容 ==================== */

typedef airy_mtx_t *airy_mtx_handle_t;
typedef airy_cond_t *airy_cond_handle_t;
typedef airy_cond_t *airy_platform_cond_t;
typedef airy_thread_t *airy_thread_handle_t;
typedef airy_sock_t *airy_sock_handle_t;

/* ==================== 额外类型定义 ==================== */

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

/* ==================== Socket 额外类型 ==================== */

typedef enum { AIRY_AF_INET, AIRY_AF_INET6, AIRY_AF_UNIX } airy_address_family_t;

typedef enum {
    AIRY_SOCK_STREAM,
    AIRY_SOCK_DGRAM,
    AIRY_SOCK_SEQPACKET
} airy_sock_type_t;

typedef struct {
    airy_address_family_t family;
    uint16_t port;
    union {
        uint8_t ipv4[4];
        uint8_t ipv6[16];
        char path[108];
    } addr;
} airy_sockaddr_t;

/* ==================== Daemon 特有函数声明 ==================== */
/* 实现在 daemons/common/src/platform_compat.c */

int airy_time_now(airy_timestamp_t *ts);
int airy_time_monotonic(airy_timestamp_t *ts);
uint64_t airy_time_to_ms(const airy_timestamp_t *ts);
void airy_time_from_ms(uint64_t ms, airy_timestamp_t *ts);
/* airy_sleep_ms 已由 commons 版 platform.h 提供，此处不重复 */
uint32_t airy_process_self(void);
uint64_t airy_thread_self(void);
int airy_thread_setname(const char *name);
int airy_thread_getname(char *name, size_t size);
int airy_mkdir(const char *path, int recursive);

/* ==================== 动态库加载 (DL) 抽象 ==================== */

#ifndef AIRY_DL_T_DEFINED
#define AIRY_DL_T_DEFINED
typedef void *airy_dl_t;
#endif

airy_dl_t airy_dl_open(const char *path);
int airy_dl_close(airy_dl_t dl);
void *airy_dl_sym(airy_dl_t dl, const char *name);
const char *airy_dl_error(void);

/* ==================== 服务器端 Socket 兼容 ==================== */

int airy_sock_init(void);
void airy_sock_cleanup(void);
airy_sock_t airy_sock_create_tcp_server(const char *host, uint16_t port);

#if AIRY_PLATFORM_POSIX
airy_sock_t airy_sock_create_unix_server(const char *path);
#endif

airy_sock_t airy_sock_accept(airy_sock_t server_fd, uint32_t timeout_ms);
ssize_t airy_sock_recv(airy_sock_t sock, void *buf, size_t len);
ssize_t airy_sock_send(airy_sock_t sock, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_PLATFORM_EXT_H */
