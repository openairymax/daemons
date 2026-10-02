// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_cfg_file.c
 * @brief Daemon 配置文件读取机制（daemon_cfg_file.h 唯一实现）。
 */

#include "daemon_cfg_file.h"

#include "airy_memory.h"
#include "cjson_helpers.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

/* 单配置文件读取上限：防异常大文件拖垮启动路径 */
#define CFG_FILE_MAX (1024 * 1024)

int daemon_ep_def(const char *sock_unix, const char *sock_win, int tcp_port,
                  daemon_ep_cfg_t *ep)
{
    if (!ep)
        return AIRY_ERR_INVALID_PARAM;
#if defined(AIRY_PLATFORM_WINDOWS)
    ep->socket_path = AIRY_STRDUP(sock_win);
#else
    ep->socket_path = AIRY_STRDUP(sock_unix);
#endif
    ep->use_tcp = 0;
    ep->tcp_port = tcp_port;
    return AIRY_SUCCESS;
}

void daemon_ep_parse(const cJSON *daemon_cfg, daemon_ep_cfg_t *ep)
{
    if (!daemon_cfg || !ep)
        return;
    const cJSON *item = cJSON_GetObjectItem(daemon_cfg, "socket_path");
    if (cJSON_IsString(item)) {
        char *dup = AIRY_STRDUP(item->valuestring);
        if (dup) {
            AIRY_FREE(ep->socket_path);
            ep->socket_path = dup;
        }
    }
    item = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
    if (cJSON_IsNumber(item) && item->valueint > 0 && item->valueint <= 65535) {
        ep->tcp_port = item->valueint;
        ep->use_tcp = 1;
    }
}

int daemon_cfg_read(const char *config_path, daemon_cfg_fn fn, void *ud)
{
    if (!fn)
        return AIRY_ERR_INVALID_PARAM;
    if (!config_path)
        return AIRY_SUCCESS;
    FILE *f = fopen(config_path, "rb");
    if (!f)
        return AIRY_SUCCESS;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < CFG_FILE_MAX) {
        char *content = (char *)AIRY_MALLOC((size_t)len + 1);
        if (content) {
            size_t read_len = fread(content, 1, (size_t)len, f);
            if (read_len == (size_t)len) {
                content[read_len] = '\0';
                CJSON_PARSE_GUARD(root, content, {
                    /* 解析失败：缺省即终态，fn 不被调用 */
                    AIRY_FREE(content);
                    fclose(f);
                    return AIRY_SUCCESS;
                });
                fn(root, ud);
            }
        }
        AIRY_FREE(content);
    }
    fclose(f);
    return AIRY_SUCCESS;
}
