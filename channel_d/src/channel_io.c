// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_io.c
 * @brief Channel service 收发与连通性探测域：channel_service_send /
 *        channel_service_receive / channel_service_ping。
 *
 * 2026-08-27 域拆分（原 channel_service.c 849 行 → 2 文件）：生命周期/
 * 打开/关闭/查询域见 channel_service.c；内部类型与 find_channel()/
 * get_time_ms() 经 channel_service_internal.h 共享。
 */

#include "channel_service.h"

#include "airy_mman.h"
#include "atomic_compat.h"
#include "daemon_errors.h"
#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "channel_service_internal.h"
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

/* P2: write() may perform a short (partial) write or return EINTR; a single
 * call can silently truncate a frame. Loop until all bytes are written. */
static ssize_t channel_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* sink closed / zero capacity */
        off += (size_t)n;
    }
    return (ssize_t)off;
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

    entry->info.last_activity = get_time_ms();
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
        struct sockaddr_un client_addr;
        __builtin_memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sun_family = AF_UNIX;
        AIRY_STRNCPY_TERM(client_addr.sun_path, endpoint, sizeof(client_addr.sun_path));
        (client_addr.sun_path)[sizeof(client_addr.sun_path) - 1] = '\0';
        int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd < 0) {
            AIRY_ERROR(AIRY_ERR_IO, "client socket creation failed");
            return AIRY_ERR_IO;
        }
        {
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(client_fd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
            close(client_fd);
            return (errno == EAGAIN || errno == ETIMEDOUT) ? AIRY_ERR_TIMEOUT : AIRY_ERR_IO;
        }
        uint32_t net_len = htonl((uint32_t)data_len);
        ssize_t w1 = channel_write_all(client_fd, &net_len, sizeof(net_len));
        ssize_t w2 = (w1 >= 0) ? channel_write_all(client_fd, data, data_len) : -1;
        close(client_fd);
        if (w1 < 0 || w2 < 0) {
            io_rc = (errno == EAGAIN || errno == ETIMEDOUT) ? AIRY_ERR_TIMEOUT : AIRY_ERR_IO;
        }
        break;
    }
    case CHANNEL_TYPE_PIPE: {
        if (endpoint[0]) {
            int fd = open(endpoint, O_WRONLY | O_NONBLOCK);
            if (fd < 0) {
                AIRY_ERROR(AIRY_ERR_IO, "pipe open for write failed");
                return AIRY_ERR_IO;
            }
            uint32_t net_len = htonl((uint32_t)data_len);
            if (channel_write_all(fd, &net_len, sizeof(net_len)) < 0 ||
                channel_write_all(fd, data, data_len) < 0) {
                close(fd);
                AIRY_ERROR(AIRY_ERR_IO, "pipe write failed");
                return AIRY_ERR_IO;
            }
            close(fd);
        }
        break;
    }
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

    entry->info.last_activity = get_time_ms();
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

    int io_rc = 0;
    switch (type) {
    case CHANNEL_TYPE_SOCKET: {
        if (socket_fd < 0) {
            return AIRY_ERR_IO;
        }
        int client_fd = accept(socket_fd, NULL, NULL);
        if (client_fd < 0) {
            return AIRY_ERR_IO;
        }
        uint32_t net_len = 0;
        ssize_t r1 = read(client_fd, &net_len, sizeof(net_len));
        if (r1 <= 0) {
            close(client_fd);
            return AIRY_ERR_IO;
        }
        uint32_t msg_len = ntohl(net_len);
        if (msg_len == 0 || msg_len > max_msg) {
            close(client_fd);
            return AIRY_ERR_OVERFLOW;
        }
        void *buf = AIRY_MALLOC(msg_len);
        if (!buf) {
            close(client_fd);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        ssize_t r2 = read(client_fd, buf, msg_len);
        close(client_fd);
        if (r2 <= 0) {
            AIRY_FREE(buf);
            return AIRY_ERR_IO;
        }
        *out_data = buf;
        *out_len = (size_t)r2;
        break;
    }
    case CHANNEL_TYPE_PIPE: {
        if (!endpoint[0]) {
            return 0;
        }
        int fd = open(endpoint, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            return 0;
        }
        uint32_t net_len = 0;
        ssize_t r1 = read(fd, &net_len, sizeof(net_len));
        if (r1 <= 0) {
            close(fd);
            return 0;
        }
        uint32_t msg_len = ntohl(net_len);
        if (msg_len == 0) {
            close(fd);
            return 0;
        }
        void *buf = AIRY_MALLOC(msg_len);
        if (!buf) {
            close(fd);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        ssize_t r2 = read(fd, buf, msg_len);
        close(fd);
        if (r2 <= 0) {
            AIRY_FREE(buf);
            return 0;
        }
        *out_data = buf;
        *out_len = (size_t)r2;
        break;
    }
    case CHANNEL_TYPE_SHM:

        break;
    default:
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown channel type in receive");
        return AIRY_ERR_UNKNOWN;
    }

    if (io_rc != 0) {
        return io_rc;
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

    uint64_t start_ms = get_time_ms();
    int rc = CHANNEL_OK;

    switch (entry->info.type) {
    case CHANNEL_TYPE_SOCKET: {
        if (entry->socket_fd < 0 || entry->info.endpoint[0] == '\0') {
            rc = AIRY_ERR_IO;
            break;
        }

        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            rc = AIRY_ERR_IO;
            break;
        }

        struct sockaddr_un addr;
        __builtin_memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        AIRY_STRNCPY_TERM(addr.sun_path, entry->info.endpoint, sizeof(addr.sun_path));
        (addr.sun_path)[sizeof(addr.sun_path) - 1] = '\0';

        {
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock_fd);
            rc = (errno == EAGAIN || errno == ETIMEDOUT || errno == EINPROGRESS) ?
                     AIRY_ERR_TIMEOUT :
                     AIRY_ERR_IO;
            break;
        }

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

    uint64_t end_ms = get_time_ms();
    *out_latency_ms = (int64_t)(end_ms - start_ms);

    if (rc == CHANNEL_OK) {
        entry->info.last_activity = end_ms;
    }

    airy_mtx_unlock(&svc->lock);
    return rc;
}
