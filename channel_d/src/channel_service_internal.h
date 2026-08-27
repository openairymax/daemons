// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_service_internal.h
 * @brief Channel service 拆分文件间的共享内部类型与声明（2026-08-27）。
 *
 * channel_service.c 按单一职责拆分为两个文件：
 *   - channel_service.c   生命周期/打开/关闭/查询域
 *   - channel_io.c        收发（send/receive）与连通性探测（ping）域
 * 内部结构体（channel_entry_t / struct channel_service）与跨文件共享的
 * find_channel()/get_time_ms() 经此头声明。
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

/* 时间戳工具（channel_service.c 定义，收发/探测域共用） */
uint64_t get_time_ms(void);

/* 按 channel_id 查找通道条目（channel_service.c 定义，各域共用） */
channel_entry_t *find_channel(channel_service_t *svc, const char *channel_id);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_CHANNEL_SERVICE_INTERNAL_H */
