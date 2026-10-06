// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_win32.c
 * @brief channel_d Windows 平台钩子与未映射传输域（G1 编译门禁窗）。
 *
 * 簿记机制（生命周期/close/查询/find_channel）在
 * channel_book.c 双平台单源；本文件仅注入 Windows 平台策略：
 *   - 三后端钩子空实现：Windows 无 socket_dir 目录职责，且条目永不持有
 *     平台资源（open 显式拒绝），空体即正确语义；
 *   - open/send/receive 返回 AIRY_ERR_NOT_SUPPORTED —— SOCKET/PIPE/SHM
 *     传输在 UCRT/airy_mman 下未映射（#124 实证），与 HTTP/2 网关
 *     Windows 禁用、run_stream 501 stub 同策略：显式"平台未映射"而非
 *     假成功；
 *   - ping 返回 latency 0（无传输可探测）。
 * AF_UNIX(Win10 17063+)/命名管道传输映射完成后删除本文件并恢复
 * channel_service.c / channel_io.c 参与 Windows 构建。
 */

#include "channel_service_internal.h"

#include "error.h"

int backend_svc_init(channel_service_t *svc)
{
    (void)svc;
    return 0;
}

void backend_svc_start(channel_service_t *svc)
{
    (void)svc;
}

void backend_entry_free(channel_entry_t *entry)
{
    (void)entry;
}

int channel_service_open(channel_service_t *svc, const char *channel_id, const char *name,
                         channel_type_t type, const char *endpoint)
{
    (void)svc;
    (void)channel_id;
    (void)name;
    (void)type;
    (void)endpoint;
    return AIRY_ERR_NOT_SUPPORTED;
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

int channel_service_ping(channel_service_t *svc, const char *channel_id, int64_t *out_latency_ms)
{
    if (!svc || !channel_id || !out_latency_ms)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    *out_latency_ms = 0;
    airy_mtx_unlock(&svc->lock);
    return CHANNEL_OK;
}
