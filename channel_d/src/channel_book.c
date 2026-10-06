// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_book.c
 * @brief Channel 服务簿记域单源：生命周期/close/查询与共享工具（双平台编译）。
 *
 * 机制与策略分离：本文件是 channel_d 的机制层——服务簿记只有唯一实现，
 * 消解 channel_win32.c 与 channel_service.c 的簿记克隆（15 windows）；
 * 平台差异经 channel_service_internal.h 声明的三个后端钩子注入：
 *   - backend_svc_init    平台运行目录自举（POSIX：逐级 mkdir；Win32：无）
 *   - backend_svc_start   平台启动前准备（POSIX：mkdir 补创；Win32：无）
 *   - backend_entry_free  平台通道资源回收（POSIX：socket/shm 清理；Win32：无）
 *
 * POSIX 策略实现见 channel_service.c；Windows 传输未映射（#124）见
 * channel_win32.c。channel_io.c 收发域经链接共享 find_channel。
 */

#include "channel_service_internal.h"

#include "airy_memory.h"
#include "daemon_errors.h"
#include "string_compat.h"

#include <string.h>
#include "error.h"

channel_entry_t *find_channel(channel_service_t *svc, const char *channel_id)
{
    if (!svc || !channel_id)
        return NULL;
    for (size_t i = 0; i < svc->channel_count; i++) {
        if (strcmp(svc->channels[i].info.channel_id, channel_id) == 0) {
            return &svc->channels[i];
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
}

void channel_entry_free(channel_entry_t *entry)
{
    backend_entry_free(entry);

    if (entry->recv_buffer) {
        AIRY_FREE(entry->recv_buffer);
        entry->recv_buffer = NULL;
    }
    entry->recv_buffer_size = 0;
    entry->recv_buffer_used = 0;
}

channel_service_t *channel_service_create(const channel_config_t *config)
{
    channel_service_t *svc = (channel_service_t *)AIRY_CALLOC(1, sizeof(channel_service_t));
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (config) {
        svc->config = *config;
    } else {
        channel_config_t defaults = CHANNEL_CONFIG_DEFAULTS;
        svc->config = defaults;
    }

    if (backend_svc_init(svc) != 0) {
        AIRY_FREE(svc);
        return NULL;
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

    if (svc->running) {
        channel_service_stop(svc);
    }

    for (size_t i = 0; i < svc->channel_count; i++) {
        channel_entry_free(&svc->channels[i]);
    }

    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int channel_service_start(channel_service_t *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;
    if (svc->running)
        return 0;

    backend_svc_start(svc);
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
        channel_entry_free(&svc->channels[i]);
    }
    svc->channel_count = 0;
    svc->running = false;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_close(channel_service_t *svc, const char *channel_id)
{
    if (!svc || !channel_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "channel not found");
        return AIRY_ERR_NOT_FOUND;
    }

    channel_entry_free(entry);
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
    if (!svc || !channel_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "channel not found");
        return AIRY_ERR_NOT_FOUND;
    }

    *out_info = entry->info;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_set_callback(channel_service_t *svc, const char *channel_id,
                                 channel_message_cb_t callback, void *user_data)
{
    if (!svc || !channel_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "channel not found");
        return AIRY_ERR_NOT_FOUND;
    }

    entry->callback = callback;
    entry->callback_user_data = user_data;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

bool channel_service_is_healthy(channel_service_t *svc)
{
    if (!svc)
        return false;
    return svc->healthy;
}
