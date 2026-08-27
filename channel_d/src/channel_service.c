// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_service.c
 * @brief Channel service 生命周期/打开/关闭/查询域：create/destroy/
 *        start/stop/open/close/list/get_info/set_callback/is_healthy，
 *        以及跨文件共享的 find_channel()/get_time_ms()。
 *
 * 2026-08-27 域拆分（原 849 行 → 2 文件）：收发与连通性探测见
 * channel_io.c；内部类型经 channel_service_internal.h 共享。
 */

#include "channel_service.h"

#include "airy_mman.h"
#include "atomic_compat.h"
#include "daemon_errors.h"
#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "channel_service_internal.h"
#include "string_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "error.h"

uint64_t get_time_ms(void)
{
    return airy_time_ms();
}

channel_entry_t *find_channel(channel_service_t *svc, const char *channel_id)
{
    for (size_t i = 0; i < svc->channel_count; i++) {
        if (strcmp(svc->channels[i].info.channel_id, channel_id) == 0) {
            return &svc->channels[i];
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
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

static int create_shm_channel(channel_entry_t *entry, const char *endpoint __attribute__((unused)))
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

static void destroy_channel(channel_entry_t *entry)
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

    /* 自举 socket_dir（SOCKET/PIPE 通道端点目录）：新系统上默认目录
     * /var/tmp/agentrt/channels 不存在，create_socket_channel() 的 bind()
     * 会因 ENOENT 失败（channel.* 返回 -32603）。daemon 自管运行目录，
     * 不依赖外部预创建——幂等逐级 mkdir。 */
    char dir_buf[256];
    size_t dir_len = strlen(svc->config.socket_dir);
    if (dir_len == 0 || dir_len >= sizeof(dir_buf)) {
        fprintf(stderr, "channel: invalid socket_dir (len=%zu)\n", dir_len);
        AIRY_FREE(svc);
        return NULL;
    }
    __builtin_memcpy(dir_buf, svc->config.socket_dir, dir_len + 1);
    for (char *p = dir_buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "channel: mkdir %s failed: %s\n", dir_buf, strerror(errno));
            AIRY_FREE(svc);
            return NULL;
        }
        *p = '/';
    }
    if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "channel: mkdir %s failed: %s\n", dir_buf, strerror(errno));
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
        destroy_channel(&svc->channels[i]);
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

    if (svc->config.socket_dir[0]) {
        mkdir(svc->config.socket_dir, 0755);
    }

    svc->running = true;
    svc->healthy = true;
    return 0;
}

int channel_service_stop(channel_service_t *svc)
{
    if (!svc || !svc->running)
        return AIRY_ERR_INVALID_PARAM;

    for (size_t i = 0; i < svc->channel_count; i++) {
        destroy_channel(&svc->channels[i]);
    }
    svc->channel_count = 0;
    svc->running = false;
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

    entry->info.created_at = get_time_ms();
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
        destroy_channel(entry);
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    svc->channel_count++;
    airy_mtx_unlock(&svc->lock);
    return 0;
}

int channel_service_close(channel_service_t *svc, const char *channel_id)
{
    if (!svc || !channel_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    for (size_t i = 0; i < svc->channel_count; i++) {
        if (strcmp(svc->channels[i].info.channel_id, channel_id) == 0) {
            destroy_channel(&svc->channels[i]);
            if (i < svc->channel_count - 1) {
                svc->channels[i] = svc->channels[svc->channel_count - 1];
                __builtin_memset(&svc->channels[svc->channel_count - 1], 0,
                                 sizeof(channel_entry_t));
                svc->channels[svc->channel_count - 1].socket_fd = -1;
                svc->channels[svc->channel_count - 1].shm_fd = -1;
            }
            svc->channel_count--;
            airy_mtx_unlock(&svc->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&svc->lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "channel not found");
    return AIRY_ERR_NOT_FOUND;
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
