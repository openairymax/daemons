// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_cfg_file.c
 * @brief Daemon 配置文件读取与端点装配机制（daemon_cfg_file.h 唯一实现）。
 */

#include "daemon_cfg_file.h"

#include "airy_memory.h"
#include "cjson_helpers.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

/* 单配置文件读取上限：防异常大文件拖垮启动路径 */
#define CFG_FILE_MAX (1024 * 1024)

typedef struct {
    daemon_ep_cfg_t *ep;
    daemon_keys_fn keys;
    void *user;
} ep_load_ctx_t;

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

static void ep_on_load(cJSON *root, void *ud)
{
    ep_load_ctx_t *ctx = (ep_load_ctx_t *)ud;
    daemon_ep_parse(cJSON_GetObjectItem(root, "daemon"), ctx->ep);
    if (ctx->keys)
        ctx->keys(root, ctx->user);
}

int daemon_ep_load(daemon_ep_cfg_t *ep, const char *config_path,
                   const char *sock_unix, const char *sock_win, int tcp_port,
                   daemon_keys_fn keys, void *user)
{
    int rc = daemon_ep_def(sock_unix, sock_win, tcp_port, ep);
    if (rc != AIRY_SUCCESS)
        return rc;
    ep_load_ctx_t ctx = {ep, keys, user};
    return daemon_cfg_read(config_path, ep_on_load, &ctx);
}

void daemon_ep_free(daemon_ep_cfg_t *ep)
{
    if (!ep)
        return;
    AIRY_FREE(ep->socket_path);
    AIRY_MEMSET(ep, 0, sizeof(*ep));
}

void daemon_ep_fill(daemon_endpoint_t *out, const daemon_ep_cfg_t *ep, int cmdline_tcp)
{
    if (!out || !ep)
        return;
    out->use_tcp = cmdline_tcp ? 1 : (ep->use_tcp ? 1 : 0);
    out->tcp_host = "127.0.0.1";
    out->tcp_port = ep->tcp_port;
    out->sock_unix = ep->socket_path;
    out->sock_win = ep->socket_path;
}

void daemon_ep_base(daemon_endpoint_t *out, int cmdline_tcp, const char *sock_unix,
                    const char *sock_win, int tcp_port)
{
    if (!out)
        return;
    out->use_tcp = cmdline_tcp ? 1 : 0;
    out->tcp_host = "127.0.0.1";
    out->tcp_port = tcp_port;
    out->sock_unix = sock_unix;
    out->sock_win = sock_win;
}

/* 进程端点槽：每 daemon 进程唯一端点配置，机制件持有。 */
static daemon_ep_cfg_t g_ep_slot;

daemon_ep_cfg_t *daemon_ep_slot(void)
{
    return &g_ep_slot;
}

void daemon_ep_apply(daemon_endpoint_t *out, int cmdline_tcp)
{
    daemon_ep_fill(out, &g_ep_slot, cmdline_tcp);
}
