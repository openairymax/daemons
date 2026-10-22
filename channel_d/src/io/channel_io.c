// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_io.c
 * @brief Channel service 收发与连通性探测域：channel_service_send /
 *        channel_service_receive / channel_service_ping。
 *
 * 2026-08-27 域拆分（原 channel_service.c 849 行 → 2 文件）：生命周期/
 * 打开/关闭/查询域见 channel_service.c；内部类型与 find_channel() 经
 * channel_service_internal.h 共享。
 */

#include "airy_mman.h"
#include "atomic_compat.h"
#include "daemon_errors.h"
#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "service/channel_service_internal.h"
#include "string_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "error.h"
#include "io.h"

/* 建立到 UNIX 端点的一次性连接：0 成功；失败返回 IO/TIMEOUT 且 fd 已关闭。 */
static int chan_connect(const char *endpoint, int with_rcv, int *out_fd)
{
    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, endpoint, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "client socket creation failed");
        return AIRY_ERR_IO;
    }
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (with_rcv)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int rc = (errno == EAGAIN || errno == ETIMEDOUT) ? AIRY_ERR_TIMEOUT : AIRY_ERR_IO;
        close(fd);
        return rc;
    }
    *out_fd = fd;
    return 0;
}

/* 连上端点并写一帧 [net_len][payload]；返回 0 / IO / TIMEOUT。 */
static int chan_sock_send(const char *endpoint, const void *data, size_t data_len)
{
    int fd = -1;
    int rc = chan_connect(endpoint, 0, &fd);
    if (rc != 0)
        return rc;
    uint32_t net_len = htonl((uint32_t)data_len);
    ssize_t w1 = airy_io_write_all(fd, &net_len, sizeof(net_len));
    ssize_t w2 = (w1 >= 0) ? airy_io_write_all(fd, data, data_len) : -1;
    close(fd);
    if (w1 >= 0 && w2 >= 0)
        return 0;
    return (errno == EAGAIN || errno == ETIMEDOUT) ? AIRY_ERR_TIMEOUT : AIRY_ERR_IO;
}

/* 以非阻塞方式向 FIFO 写一帧；成功 0，打开/写失败返回 IO。 */
static int chan_pipe_send(const char *endpoint, const void *data, size_t data_len)
{
    if (!endpoint[0])
        return 0;
    int fd = open(endpoint, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "pipe open for write failed");
        return AIRY_ERR_IO;
    }
    uint32_t net_len = htonl((uint32_t)data_len);
    if (airy_io_write_all(fd, &net_len, sizeof(net_len)) < 0 ||
        airy_io_write_all(fd, data, data_len) < 0) {
        close(fd);
        AIRY_ERROR(AIRY_ERR_IO, "pipe write failed");
        return AIRY_ERR_IO;
    }
    close(fd);
    return 0;
}

/* 读一帧 [net_len][payload]；zero_rc 覆盖读空与零长，len > max_len 报溢出。 */
static int chan_read_frame(int fd, uint32_t max_len, int zero_rc, void **out, size_t *out_len)
{
    uint32_t net_len = 0;
    if (read(fd, &net_len, sizeof(net_len)) <= 0)
        return zero_rc;
    uint32_t len = ntohl(net_len);
    if (len > max_len)
        return AIRY_ERR_OVERFLOW;
    if (len == 0)
        return zero_rc;
    void *buf = AIRY_MALLOC(len);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    ssize_t got = read(fd, buf, len);
    if (got <= 0) {
        AIRY_FREE(buf);
        return zero_rc;
    }
    *out = buf;
    *out_len = (size_t)got;
    return 0;
}

int channel_service_send(channel_service_t *svc, const char *channel_id, const void *data,
                         size_t data_len)
{
    if (!svc || !channel_id || !data || data_len == 0)
        return AIRY_ERR_INVALID_PARAM;

    char endpoint[512];
    int socket_fd = -1;
    void *shm_ptr = NULL;
    size_t shm_size = 0;
    int type = -1;
    channel_message_cb_t cb = NULL;
    void *ud = NULL;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry || entry->info.status != CHANNEL_STATUS_OPEN) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_NOT_FOUND, "channel not found or not open");
        return AIRY_ERR_NOT_FOUND;
    }

    entry->info.last_activity = airy_time_ms();
    type = entry->info.type;
    cb = entry->callback;
    ud = entry->callback_user_data;
    AIRY_STRNCPY_TERM(endpoint, entry->info.endpoint, sizeof(endpoint));
    socket_fd = entry->socket_fd;
    shm_ptr = entry->shm_ptr;
    shm_size = entry->shm_size;

    if (type == CHANNEL_TYPE_SHM) {
        if (!shm_ptr) {
            airy_mtx_unlock(&svc->lock);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "shm_ptr is NULL in send");
            return AIRY_ERR_UNKNOWN;
        }
        size_t header_size = sizeof(uint32_t) * 2;
        /* Check header fits the buffer first, then test data_len via
         * subtraction to avoid data_len + header_size wrap-around. */
        if (header_size > shm_size) {
            airy_mtx_unlock(&svc->lock);
            AIRY_ERROR(AIRY_ERR_OVERFLOW, "shm buffer too small for header");
            return AIRY_ERR_OVERFLOW;
        }
        if (data_len > shm_size - header_size) {
            airy_mtx_unlock(&svc->lock);
            AIRY_ERROR(AIRY_ERR_OVERFLOW, "data exceeds shm buffer size");
            return AIRY_ERR_OVERFLOW;
        }
        volatile uint32_t *msg_len = (volatile uint32_t *)shm_ptr;
        volatile uint32_t *msg_flag = (volatile uint32_t *)((char *)shm_ptr + sizeof(uint32_t));
        *msg_len = (uint32_t)data_len;
        __builtin_memcpy((char *)shm_ptr + header_size, data, data_len);
        atomic_thread_fence(memory_order_seq_cst);
        *msg_flag = 1;
    }
    airy_mtx_unlock(&svc->lock);

    int io_rc = 0;
    switch (type) {
    case CHANNEL_TYPE_SOCKET: {
        if (socket_fd < 0) {
            AIRY_ERROR(AIRY_ERR_IO, "socket not open for send");
            return AIRY_ERR_IO;
        }
        io_rc = chan_sock_send(endpoint, data, data_len);
        break;
    }
    case CHANNEL_TYPE_PIPE:
        io_rc = chan_pipe_send(endpoint, data, data_len);
        break;
    case CHANNEL_TYPE_SHM:

        break;
    default:
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown channel type in send");
        return AIRY_ERR_UNKNOWN;
    }

    if (io_rc != 0) {
        return io_rc;
    }

    airy_mtx_lock(&svc->lock);
    channel_entry_t *e2 = find_channel(svc, channel_id);
    if (e2 && e2->info.status == CHANNEL_STATUS_OPEN) {
        e2->info.messages_sent++;
        svc->total_messages_sent++;
    }
    airy_mtx_unlock(&svc->lock);

    if (cb) {
        cb(channel_id, data, data_len, ud);
    }
    return 0;
}

int channel_service_receive(channel_service_t *svc, const char *channel_id, void **out_data,
                            size_t *out_len)
{
    if (!svc || !channel_id || !out_data || !out_len)
        return AIRY_ERR_INVALID_PARAM;

    char endpoint[512];
    int socket_fd = -1;
    void *shm_ptr = NULL;
    size_t shm_size = 0;
    uint32_t max_msg = 0;
    int type = -1;

    airy_mtx_lock(&svc->lock);
    channel_entry_t *entry = find_channel(svc, channel_id);
    if (!entry || entry->info.status != CHANNEL_STATUS_OPEN) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    entry->info.last_activity = airy_time_ms();
    type = entry->info.type;
    AIRY_STRNCPY_TERM(endpoint, entry->info.endpoint, sizeof(endpoint));
    socket_fd = entry->socket_fd;
    shm_ptr = entry->shm_ptr;
    shm_size = entry->shm_size;
    max_msg = svc->config.default_buffer_size;

    if (type == CHANNEL_TYPE_SHM) {
        if (!shm_ptr) {
            airy_mtx_unlock(&svc->lock);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "shm_ptr is NULL in receive");
            return AIRY_ERR_UNKNOWN;
        }
        volatile uint32_t *msg_len = (volatile uint32_t *)shm_ptr;
        volatile uint32_t *msg_flag = (volatile uint32_t *)((char *)shm_ptr + sizeof(uint32_t));
        atomic_thread_fence(memory_order_seq_cst);
        if (*msg_flag != 1) {
            airy_mtx_unlock(&svc->lock);
            return 0;
        }
        uint32_t len = *msg_len;
        if (len == 0 || len > shm_size - sizeof(uint32_t) * 2) {
            airy_mtx_unlock(&svc->lock);
            AIRY_ERROR(AIRY_ERR_OVERFLOW, "shm message length overflow");
            return AIRY_ERR_OVERFLOW;
        }
        void *buf = AIRY_MALLOC(len);
        if (!buf) {
            airy_mtx_unlock(&svc->lock);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(buf, (char *)shm_ptr + sizeof(uint32_t) * 2, len);
        atomic_thread_fence(memory_order_seq_cst);
        *msg_flag = 0;
        *out_data = buf;
        *out_len = len;
    }
    airy_mtx_unlock(&svc->lock);

    switch (type) {
    case CHANNEL_TYPE_SOCKET: {
        if (socket_fd < 0)
            return AIRY_ERR_IO;
        int client_fd = accept(socket_fd, NULL, NULL);
        if (client_fd < 0)
            return AIRY_ERR_IO;
        int rc = chan_read_frame(client_fd, max_msg, AIRY_ERR_IO, out_data, out_len);
        close(client_fd);
        if (rc != 0)
            return rc;
        break;
    }
    case CHANNEL_TYPE_PIPE: {
        if (!endpoint[0])
            return 0;
        int fd = open(endpoint, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            return 0;
        int rc = chan_read_frame(fd, UINT32_MAX, 0, out_data, out_len);
        close(fd);
        if (rc != 0)
            return rc;
        break;
    }
    case CHANNEL_TYPE_SHM:

        break;
    default:
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown channel type in receive");
        return AIRY_ERR_UNKNOWN;
    }

    airy_mtx_lock(&svc->lock);
    channel_entry_t *e2 = find_channel(svc, channel_id);
    if (e2 && e2->info.status == CHANNEL_STATUS_OPEN) {
        e2->info.messages_received++;
        svc->total_messages_received++;
    }
    airy_mtx_unlock(&svc->lock);
    return 0;
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

    uint64_t start_ms = airy_time_ms();
    int rc = CHANNEL_OK;

    switch (entry->info.type) {
    case CHANNEL_TYPE_SOCKET: {
        if (entry->socket_fd < 0 || entry->info.endpoint[0] == '\0') {
            rc = AIRY_ERR_IO;
            break;
        }

        int sock_fd = -1;
        rc = chan_connect(entry->info.endpoint, 1, &sock_fd);
        if (rc != 0)
            break;
        close(sock_fd);
        break;
    }
    case CHANNEL_TYPE_SHM: {
        if (!entry->shm_ptr) {
            rc = AIRY_ERR_IO;
            break;
        }
        volatile uint32_t *header = (volatile uint32_t *)entry->shm_ptr;
        atomic_thread_fence(memory_order_seq_cst);
        (void)header[0];
        break;
    }
    case CHANNEL_TYPE_PIPE: {
        if (entry->info.endpoint[0]) {
            int fd = open(entry->info.endpoint, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                rc = AIRY_ERR_IO;
            } else {
                close(fd);
            }
        } else {
            rc = AIRY_ERR_IO;
        }
        break;
    }
    default:
        rc = AIRY_ERR_PERMISSION_DENIED;
        break;
    }

    uint64_t end_ms = airy_time_ms();
    *out_latency_ms = (int64_t)(end_ms - start_ms);

    if (rc == CHANNEL_OK) {
        entry->info.last_activity = end_ms;
    }

    airy_mtx_unlock(&svc->lock);
    return rc;
}
