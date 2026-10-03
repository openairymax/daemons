// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_service_internal.h
 * @brief Channel service 拆分文件间的共享内部类型与声明。
 *
 * 簿记机制单源（channel_book.c，双平台编译）：生命周期/close/查询域与
 * find_channel()/get_time_ms()/channel_entry_free()。
 * 平台策略按钩子注入（机制与策略分离）：
 *   - channel_service.c   POSIX：三后端钩子 + SOCKET/SHM/PIPE 打开域
 *   - channel_win32.c     Win32：钩子空实现 + 未映射传输显式拒绝（#124）
 *   - channel_io.c        收发（send/receive）与连通性探测（ping）域
 */

#ifndef AIRY_RT_CHANNEL_SERVICE_INTERNAL_H
#define AIRY_RT_CHANNEL_SERVICE_INTERNAL_H

#include "channel_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    channel_info_t info;
    int socket_fd;
    void *shm_ptr;
    size_t shm_size;
    char shm_name[128];
    int shm_fd;
    channel_message_cb_t callback;
    void *callback_user_data;
    uint8_t *recv_buffer;
    size_t recv_buffer_size;
    size_t recv_buffer_used;
} channel_entry_t;

struct channel_service {
    channel_config_t config;
    channel_entry_t channels[CHANNEL_MAX_CHANNELS];
    size_t channel_count;
    bool running;
    bool healthy;
    uint64_t total_messages_sent;
    uint64_t total_messages_received;
    airy_mtx_t lock;
};

/* 时间戳工具（channel_book.c 定义，收发/探测域共用） */
uint64_t get_time_ms(void);

/* 按 channel_id 查找通道条目（channel_book.c 定义，各域共用） */
channel_entry_t *find_channel(channel_service_t *svc, const char *channel_id);

/* 簿记释放：先回收平台资源（backend_entry_free）再释放接收缓冲。
 * channel_book.c 定义，stop/close/destroy 与 open 失败回滚共用。 */
void channel_entry_free(channel_entry_t *entry);

/* 后端钩子（平台文件实现，channel_book.c 经此注入平台策略）：
 * 平台运行目录自举，非 0 表示失败；平台启动前准备；条目平台资源回收。 */
int backend_svc_init(channel_service_t *svc);
void backend_svc_start(channel_service_t *svc);
void backend_entry_free(channel_entry_t *entry);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_CHANNEL_SERVICE_INTERNAL_H */
