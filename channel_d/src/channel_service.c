// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_service.c
 * @brief Channel 服务 POSIX 策略注入：三后端钩子与 SOCKET/SHM/PIPE 打开域。
 *
 * 生命周期/close/查询簿记已下沉 channel_book.c 双平台单源；本文件是
 * POSIX 策略面：
 *   - backend_svc_init/start  socket_dir 逐级 mkdir 自举与补创
 *   - backend_entry_free      socket/shm 平台资源回收
 *   - channel_service_open    三传输建立（AF_UNIX SOCKET / shm_open SHM /
 *                             mkfifo PIPE）
 * 收发与连通性探测见 channel_io.c；Windows 策略见 channel_win32.c。
 */

#include "airy_mman.h"
#include "daemon_errors.h"
#include "airy_memory.h"
#include "channel_service_internal.h"
#include "string_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "error.h"

int backend_svc_init(channel_service_t *svc)
{
    /* 自举 socket_dir（SOCKET/PIPE 通道端点目录）：新系统上默认目录
     * /var/tmp/agentrt/channels 不存在，create_socket_channel() 的 bind()
     * 会因 ENOENT 失败（channel.* 返回 -32603）。daemon 自管运行目录，
     * 不依赖外部预创建——幂等逐级 mkdir。 */
    char dir_buf[256];
    size_t dir_len = strlen(svc->config.socket_dir);
    if (dir_len == 0 || dir_len >= sizeof(dir_buf)) {
        fprintf(stderr, "channel: invalid socket_dir (len=%zu)\n", dir_len);
        return -1;
    }
    __builtin_memcpy(dir_buf, svc->config.socket_dir, dir_len + 1);
    for (char *p = dir_buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "channel: mkdir %s failed: %s\n", dir_buf, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "channel: mkdir %s failed: %s\n", dir_buf, strerror(errno));
        return -1;
    }
    return 0;
}

void backend_svc_start(channel_service_t *svc)
{
    if (svc->config.socket_dir[0]) {
        mkdir(svc->config.socket_dir, 0755);
    }
}

void backend_entry_free(channel_entry_t *entry)
{
    if (entry->socket_fd >= 0) {
        close(entry->socket_fd);
        if (entry->info.type == CHANNEL_TYPE_SOCKET && entry->info.endpoint[0]) {
            unlink(entry->info.endpoint);
        }
        entry->socket_fd = -1;
    }

    if (entry->shm_ptr && entry->shm_ptr != MAP_FAILED) {
        munmap(entry->shm_ptr, entry->shm_size);
        entry->shm_ptr = NULL;
    }
    if (entry->shm_fd >= 0) {
        close(entry->shm_fd);
        entry->shm_fd = -1;
    }
    if (entry->shm_name[0]) {
        shm_unlink(entry->shm_name);
        entry->shm_name[0] = '\0';
    }
}

static int create_socket_channel(channel_entry_t *entry, const char *endpoint)
{
    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, endpoint, sizeof(addr.sun_path));
    (addr.sun_path)[sizeof(addr.sun_path) - 1] = '\0';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "socket creation failed");
        return AIRY_ERR_IO;
    }

    unlink(endpoint);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        AIRY_ERROR(AIRY_ERR_IO, "bind failed on endpoint");
        return AIRY_ERR_IO;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        unlink(endpoint);
        AIRY_ERROR(AIRY_ERR_IO, "listen failed on endpoint");
        return AIRY_ERR_IO;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    entry->socket_fd = fd;
    return 0;
}

static int create_shm_channel(channel_entry_t *entry, const char *endpoint)
{

    char channel_id_copy[sizeof(entry->shm_name)];
    snprintf(channel_id_copy, sizeof(channel_id_copy), "%s", entry->info.channel_id);
    snprintf(entry->shm_name, sizeof(entry->shm_name), "%s%s", "/airy_ch_", channel_id_copy);

    size_t shm_size = entry->info.buffer_size > 0 ? entry->info.buffer_size : 65536;

    int fd = shm_open(entry->shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        AIRY_ERROR(AIRY_ERR_IO, "shm_open failed");
        return AIRY_ERR_IO;
    }

    if (ftruncate(fd, (off_t)shm_size) < 0) {
        close(fd);
        shm_unlink(entry->shm_name);
        AIRY_ERROR(AIRY_ERR_IO, "ftruncate failed on shm");
        return AIRY_ERR_IO;
    }

    void *ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        shm_unlink(entry->shm_name);
        AIRY_ERROR(AIRY_ERR_IO, "mmap failed on shm");
        return AIRY_ERR_IO;
    }

    __builtin_memset(ptr, 0, shm_size);

    entry->shm_fd = fd;
    entry->shm_ptr = ptr;
    entry->shm_size = shm_size;
    return 0;
}

int channel_service_open(channel_service_t *svc, const char *channel_id, const char *name,
                         channel_type_t type, const char *endpoint)
{
    if (!svc || !channel_id || !name)
        return AIRY_ERR_INVALID_PARAM;
    if (svc->channel_count >= svc->config.max_channels) {
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "channel count limit reached");
        return AIRY_ERR_OVERFLOW;
    }

    airy_mtx_lock(&svc->lock);
    if (find_channel(svc, channel_id)) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel already exists");
        return AIRY_ERR_UNKNOWN;
    }

    channel_entry_t *entry = &svc->channels[svc->channel_count];
    __builtin_memset(entry, 0, sizeof(channel_entry_t));
    entry->socket_fd = -1;
    entry->shm_fd = -1;

    AIRY_STRNCPY_TERM(entry->info.channel_id, channel_id, sizeof(entry->info.channel_id));
    AIRY_STRNCPY_TERM(entry->info.name, name, sizeof(entry->info.name));
    entry->info.type = type;
    entry->info.status = CHANNEL_STATUS_OPEN;
    entry->info.buffer_size = svc->config.default_buffer_size;

    if (endpoint) {
        AIRY_STRNCPY_TERM(entry->info.endpoint, endpoint, sizeof(entry->info.endpoint));
    } else {
        if (type == CHANNEL_TYPE_SOCKET) {
            snprintf(entry->info.endpoint, sizeof(entry->info.endpoint), "%s/%s.sock",
                     svc->config.socket_dir, channel_id);
        } else if (type == CHANNEL_TYPE_SHM) {
            snprintf(entry->info.endpoint, sizeof(entry->info.endpoint), "%s%s",
                     svc->config.shm_prefix, channel_id);
        }
    }

    entry->info.created_at = airy_time_ms();
    entry->info.last_activity = entry->info.created_at;

    int rc = 0;
    switch (type) {
    case CHANNEL_TYPE_SOCKET:
        rc = create_socket_channel(entry, entry->info.endpoint);
        break;
    case CHANNEL_TYPE_SHM:
        rc = create_shm_channel(entry, entry->info.endpoint);
        break;
    case CHANNEL_TYPE_PIPE:
        if (entry->info.endpoint[0]) {
            rc = mkfifo(entry->info.endpoint, 0666);
            if (rc < 0 && errno != EEXIST)
                rc = AIRY_ERR_IO;
            else
                rc = 0;
        }
        break;
    default:
        rc = AIRY_ERR_NOT_SUPPORTED;
        break;
    }

    if (rc < 0) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_IO, "channel creation failed");
        return AIRY_ERR_IO;
    }

    entry->recv_buffer_size = entry->info.buffer_size;
    entry->recv_buffer = (uint8_t *)AIRY_CALLOC(1, entry->recv_buffer_size);
    if (!entry->recv_buffer) {
        channel_entry_free(entry);
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    svc->channel_count++;
    airy_mtx_unlock(&svc->lock);
    return 0;
}
