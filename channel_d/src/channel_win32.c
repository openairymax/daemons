// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_win32.c
 * @brief channel_d Windows 实现（G1 编译门禁窗）。
 *
 * 三种通道后端在 Windows 的现状（#124 实证）：
 *   - SOCKET：POSIX AF_UNIX socket（channel_service.c create_socket_channel /
 *     channel_io.c send/receive/ping），UCRT 无 sys/socket.h/sys/un.h；
 *   - PIPE：mkfifo/fifo open（channel_service.c open / channel_io.c）；
 *   - SHM：shm_open/mmap（airy_mman.h Windows 分支为 stub 恒 -1）。
 *
 * 服务层（create/start/stop/list/get_info/set_callback/is_healthy/close）是
 * 纯内存簿记，跨平台可正常提供；打开任意通道后端均返回
 * AIRY_ERR_NOT_SUPPORTED —— 与 HTTP/2 网关 Windows 禁用、run_stream 501
 * stub 同策略：显式"平台未映射"而非假成功。AF_UNIX(Win10 17063+)/命名管道
 * 通道传输映射完成后再切换真实实现（届时删本文件并恢复 channel_service.c /
 * channel_io.c 参与 Windows 构建）。
 *
 * 仅 Windows 编译（channel_d/CMakeLists.txt WIN32 分支替换源清单）。
 */

#include "channel_service_internal.h"

#include "airy_memory.h"
#include "error.h"

#include <string.h>

uint64_t get_time_ms(void)
{
    return airy_time_ms();
}

channel_entry_t *find_channel(channel_service_t *svc, const char *channel_id)
{
    if (!svc || !channel_id)
        return NULL;
    for (size_t i = 0; i < svc->channel_count; i++) {
        if (strcmp(svc->channels[i].info.channel_id, channel_id) == 0)
            return &svc->channels[i];
    }
    return NULL;
}

channel_service_t *channel_service_create(const channel_config_t *config)
{
    channel_service_t *svc = (channel_service_t *)AIRY_CALLOC(1, sizeof(channel_service_t));
    if (!svc)
        return NULL;

    if (config) {
        svc->config = *config;
    } else {
        channel_config_t defaults = CHANNEL_CONFIG_DEFAULTS;
        svc->config = defaults;
    }

    for (size_t i = 0; i < CHANNEL_MAX_CHANNELS; i++) {
        svc->channels[i].socket_fd = -1;
        svc->channels[i].shm_fd = -1;
    }

    svc->healthy = true;
    airy_mtx_init(&svc->lock);
    return svc;
}

void channel_service_destroy(channel_service_t *svc)
{
    if (!svc)
        return;
    if (svc->running)
        channel_service_stop(svc);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int channel_service_start(channel_service_t *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;
    if (svc->running)
        return 0;
    svc->running = true;
    svc->healthy = true;
    return 0;
}

int channel_service_stop(channel_service_t *svc)
{
    if (!svc || !svc->running)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    for (size_t i = 0; i < svc->channel_count; i++) {
        if (svc->channels[i].recv_buffer) {
            AIRY_FREE(svc->channels[i].recv_buffer);
            svc->channels[i].recv_buffer = NULL;
        }
        AIRY_FREE(svc->channels[i].shm_ptr);
        svc->channels[i].shm_ptr = NULL;
        svc->channels[i].shm_name[0] = '\0';
        svc->channels[i].socket_fd = -1;
        svc->channels[i].shm_fd = -1;
    }
    svc->channel_count = 0;
    svc->running = false;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_open(channel_service_t *svc, const char *channel_id, const char *name,
                         channel_type_t type, const char *endpoint)
{
    (void)svc;
    (void)channel_id;
    (void)name;
    (void)type;
    (void)endpoint;
    /* Windows：SOCKET/PIPE/SHM 后端传输均未映射（见文件头），显式拒绝。 */
    return AIRY_ERR_NOT_SUPPORTED;
}

int channel_service_close(channel_service_t *svc, const char *channel_id)
{
    channel_entry_t *entry;
    if (!svc || !channel_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    if (entry->recv_buffer) {
        AIRY_FREE(entry->recv_buffer);
        entry->recv_buffer = NULL;
    }
    entry->info.status = CHANNEL_STATUS_CLOSED;
    entry->info.endpoint[0] = '\0';
    /* 从数组移除（保持尾部收缩语义与 POSIX 实现一致） */
    size_t idx = (size_t)(entry - svc->channels);
    if (idx < svc->channel_count - 1) {
        svc->channels[idx] = svc->channels[svc->channel_count - 1];
    }
    __builtin_memset(&svc->channels[svc->channel_count - 1], 0, sizeof(channel_entry_t));
    svc->channels[svc->channel_count - 1].socket_fd = -1;
    svc->channels[svc->channel_count - 1].shm_fd = -1;
    svc->channel_count--;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_send(channel_service_t *svc, const char *channel_id, const void *data,
                         size_t data_len)
{
    (void)svc;
    (void)channel_id;
    (void)data;
    (void)data_len;
    return AIRY_ERR_NOT_SUPPORTED;
}

int channel_service_receive(channel_service_t *svc, const char *channel_id, void **out_data,
                            size_t *out_len)
{
    (void)svc;
    (void)channel_id;
    (void)out_data;
    (void)out_len;
    return AIRY_ERR_NOT_SUPPORTED;
}

int channel_service_list(channel_service_t *svc, channel_info_t *out_list, size_t list_capacity,
                         size_t *out_count)
{
    if (!svc || !out_list || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    size_t count = svc->channel_count;
    if (count > list_capacity)
        count = list_capacity;
    for (size_t i = 0; i < count; i++) {
        out_list[i] = svc->channels[i].info;
    }
    *out_count = count;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_get_info(channel_service_t *svc, const char *channel_id,
                             channel_info_t *out_info)
{
    channel_entry_t *entry;
    if (!svc || !channel_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    *out_info = entry->info;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_set_callback(channel_service_t *svc, const char *channel_id,
                                 channel_message_cb_t callback, void *user_data)
{
    channel_entry_t *entry;
    if (!svc || !channel_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    entry->callback = callback;
    entry->callback_user_data = user_data;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_ping(channel_service_t *svc, const char *channel_id, int64_t *out_latency_ms)
{
    channel_entry_t *entry;
    if (!svc || !channel_id || !out_latency_ms)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    *out_latency_ms = 0;
    airy_mtx_unlock(&svc->lock);
    return CHANNEL_OK;
}

bool channel_service_is_healthy(channel_service_t *svc)
{
    if (!svc)
        return false;
    return svc->healthy;
}
