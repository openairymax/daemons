/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file net.c
 * @brief notify_d 协议域（gen5 手写层：WS 握手 / 首包分派 / 连接线程）。
 * @details 单端口协议嗅探链：先经 notify_d_dispatch_jsonrpc 匹配
 *        JSON-RPC 方法（gateway 剥 <ns>. 前缀后的裸名），未命中则按
 *        HTTP 头嗅探 Upgrade(websocket) / Accept(SSE) / X-Topic(裸消息)。
 *        WS 握手自带 SHA1+Base64 精简实现（协议面专用，无外部
 *        crypto 依赖）。每连接一线程（detached），慢连接首包 poll
 *        限时 5s，不阻塞 accept 循环。
 */

#include "airy_memory.h"
#include "error.h"
#include "notify_d_internal.h"
#include "daemon_main.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define NOTIFY_D_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

atomic_int g_conns = 0;

static int notify_ws_accept_key(const char *client_key, char *out_key, size_t out_size)
{
    if (!client_key || !out_key || out_size < 64) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", client_key, NOTIFY_D_WS_GUID);

    unsigned char sha1[20];
    AIRY_MEMSET(sha1, 0, sizeof(sha1));

    unsigned int h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    unsigned int h3 = 0x10325476, h4 = 0xC3D2E1F0;

    size_t msg_len = strlen(combined);
    size_t padded_len = ((msg_len + 8) / 64 + 1) * 64;
    unsigned char *padded = (unsigned char *)AIRY_CALLOC(1, padded_len);
    if (!padded) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "calloc failed for SHA1 padded buffer");
    }
    AIRY_MEMCPY(padded, combined, msg_len);
    padded[msg_len] = 0x80;

    uint64_t bit_len = (uint64_t)msg_len * 8;
    for (int i = 0; i < 8; i++)
        padded[padded_len - 8 + i] = (unsigned char)(bit_len >> (56 - 8 * i));

    for (size_t off = 0; off < padded_len; off += 64) {
        unsigned int w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((unsigned int)padded[off + i * 4] << 24) |
                   ((unsigned int)padded[off + i * 4 + 1] << 16) |
                   ((unsigned int)padded[off + i * 4 + 2] << 8) |
                   ((unsigned int)padded[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        unsigned int a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            unsigned int f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            unsigned int temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    AIRY_FREE(padded);
    padded = NULL;

    sha1[0] = (unsigned char)(h0 >> 24);
    sha1[1] = (unsigned char)(h0 >> 16);
    sha1[2] = (unsigned char)(h0 >> 8);
    sha1[3] = (unsigned char)(h0);
    sha1[4] = (unsigned char)(h1 >> 24);
    sha1[5] = (unsigned char)(h1 >> 16);
    sha1[6] = (unsigned char)(h1 >> 8);
    sha1[7] = (unsigned char)(h1);
    sha1[8] = (unsigned char)(h2 >> 24);
    sha1[9] = (unsigned char)(h2 >> 16);
    sha1[10] = (unsigned char)(h2 >> 8);
    sha1[11] = (unsigned char)(h2);
    sha1[12] = (unsigned char)(h3 >> 24);
    sha1[13] = (unsigned char)(h3 >> 16);
    sha1[14] = (unsigned char)(h3 >> 8);
    sha1[15] = (unsigned char)(h3);
    sha1[16] = (unsigned char)(h4 >> 24);
    sha1[17] = (unsigned char)(h4 >> 16);
    sha1[18] = (unsigned char)(h4 >> 8);
    sha1[19] = (unsigned char)(h4);

    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t off = 0;

    for (int i = 0; i < 18 && off + 4 <= out_size; i += 3) {
        unsigned int val = ((unsigned int)sha1[i] << 16) | ((unsigned int)sha1[i + 1] << 8) |
                           (unsigned int)sha1[i + 2];
        out_key[off++] = b64[(val >> 18) & 0x3F];
        out_key[off++] = b64[(val >> 12) & 0x3F];
        out_key[off++] = b64[(val >> 6) & 0x3F];
        out_key[off++] = b64[val & 0x3F];
    }

    if (off + 4 <= out_size) {
        unsigned int val = ((unsigned int)sha1[18] << 16) | ((unsigned int)sha1[19] << 8);
        out_key[off++] = b64[(val >> 18) & 0x3F];
        out_key[off++] = b64[(val >> 12) & 0x3F];
        out_key[off++] = b64[(val >> 6) & 0x3F];
        out_key[off++] = '=';
    }
    out_key[off] = '\0';

    return 0;
}

static int notify_d_ws_upgrade(notify_d_service_t *svc, notify_client_t *client,
                               const char *request)
{
    (void)svc;
    const char *key_tag = "Sec-WebSocket-Key: ";
    const char *key_start = strstr(request, key_tag);
    if (!key_start) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing Sec-WebSocket-Key header");
    }
    key_start += strlen(key_tag);

    const char *key_end = strstr(key_start, "\r\n");
    if (!key_end) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "Sec-WebSocket-Key value not terminated by CRLF");
    }

    char client_key[256];
    size_t key_len = (size_t)(key_end - key_start);
    if (key_len >= sizeof(client_key)) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "Sec-WebSocket-Key too long");
    }
    AIRY_MEMCPY(client_key, key_start, key_len);
    client_key[key_len] = '\0';

    char accept_key[64];
    if (notify_ws_accept_key(client_key, accept_key, sizeof(accept_key)) != 0) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to compute WebSocket accept key");
    }

    char response[1024];
    int resp_len = snprintf(response, sizeof(response),
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n"
                            "\r\n",
                            accept_key);

    if (airy_sock_send(client->fd, response, (size_t)resp_len) <= 0) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to send WebSocket 101 response");
    }

    client->type = NOTIFY_CLIENT_WEBSOCKET;
    client->handshake_done = 1;
    return 0;
}

static notify_client_t *notify_d_find_client_slot(notify_d_service_t *svc)
{
    for (size_t i = 0; i < NOTIFY_D_MAX_CLIENTS; i++) {
        if (!svc->clients[i].active)
            return &svc->clients[i];
    }
    if (svc->client_count < NOTIFY_D_MAX_CLIENTS)
        return &svc->clients[svc->client_count];
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "max clients reached");
}

static void notify_d_handle_request(notify_d_service_t *svc, airy_sock_t client_fd);

/* 连接处理线程（detached）：每连接独立处理。notify_d_handle_request 在
 * recv 前 poll 等待首包最多 5s，单线程 accept 循环下慢连接会阻塞全部
 * RPC（health_check 偶发超时，实测 max~4.5s）。SSE/WebSocket 长连接
 * 注册进 svc->clients 后由事件线程推送，与本线程无关。arg 为 accept
 * 循环装入的 notify_conn_arg_t（fd + face），按 face 分派 notify 或
 * hook 面，处理完释放参数块。 */
void *notify_d_conn_thread(void *arg)
{
    notify_conn_arg_t *carg = (notify_conn_arg_t *)arg;
    notify_d_service_t *svc = &g_service;
    if (carg->face == NOTIFY_FACE_HOOK)
        hook_svc_serve_conn(carg->fd);
    else
        notify_d_handle_request(svc, carg->fd);
    AIRY_FREE(carg);
    atomic_fetch_sub_explicit(&g_conns, 1, memory_order_relaxed);
    return NULL;
}

static void notify_d_handle_request(notify_d_service_t *svc, airy_sock_t client_fd)
{
    char buffer[NOTIFY_D_MAX_BUFFER];
#ifndef _WIN32
    /* Wait for request data to be ready before recv: airy_sock_recv is a
     * non-blocking MSG_DONTWAIT read; recv immediately after accept returns 0
     * on EAGAIN and would misjudge the connection as failed and close it (RPC
     * timing race). */
    struct pollfd pfd;
    pfd.fd = (int)client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        airy_sock_close(client_fd);
        return;
    }
#endif
    ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        airy_sock_close(client_fd);
        return;
    }
    buffer[n] = '\0';

    /* L2 namespace methods (02-l2-service-protocol.md §6.1): gateway strips
     * the <ns>. prefix during forwarding, so here match publish/subscribe/
     * unsubscribe/list/health and the standard methods shutdown/get_stats/
     * health_check directly; on a hit respond with JSON-RPC 2.0; everything
     * else (SSE/WebSocket upgrade/plain message delivery) keeps the original
     * logic, backward compatible. */
    char rpc_response[NOTIFY_D_MAX_BUFFER];
    int dispatch_rc = notify_d_dispatch_jsonrpc(svc, buffer, rpc_response, sizeof(rpc_response));
    if (dispatch_rc == NOTIFY_D_METHOD_SHUTDOWN) {

        atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
    }
    if (dispatch_rc == NOTIFY_D_METHOD_HANDLED || dispatch_rc == NOTIFY_D_METHOD_SHUTDOWN) {
        airy_sock_send(client_fd, rpc_response, strlen(rpc_response));
        airy_sock_close(client_fd);
        return;
    }

    airy_mtx_lock(&svc->lock);

    int is_upgrade = (strstr(buffer, "Upgrade: websocket") != NULL ||
                      strstr(buffer, "Upgrade: WebSocket") != NULL);
    int is_sse = (strstr(buffer, "Accept: text/event-stream") != NULL);

    notify_client_t *client = notify_d_find_client_slot(svc);
    if (!client) {
        airy_mtx_unlock(&svc->lock);
        const char *busy = "{\"error\":\"max_clients_reached\"}";
        airy_sock_send(client_fd, busy, strlen(busy));
        airy_sock_close(client_fd);
        return;
    }

    AIRY_MEMSET(client, 0, sizeof(*client));
    client->fd = client_fd;
    client->connected_at = (uint64_t)time(NULL);
    client->last_activity = client->connected_at;
    client->active = 1;

    const char *cid_tag = "X-Client-Id: ";
    const char *cid_hdr = strstr(buffer, cid_tag);
    if (cid_hdr) {
        const char *cid_start = cid_hdr + strlen(cid_tag);
        const char *cid_end = strstr(cid_start, "\r\n");
        if (cid_end && cid_end > cid_start) {
            size_t clen = (size_t)(cid_end - cid_start);
            if (clen > 0 && clen < 256) {
                char *cid = (char *)AIRY_MALLOC(clen + 1);
                if (cid) {
                    AIRY_MEMCPY(cid, cid_start, clen);
                    cid[clen] = '\0';
                    client->client_id = cid;
                }
            }
        }
    }

    if (is_sse) {
        client->type = NOTIFY_CLIENT_SSE;
        const char *sse_headers = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n";
        airy_sock_send(client_fd, sse_headers, strlen(sse_headers));
        client->handshake_done = 1;
        svc->client_count++;
    } else if (is_upgrade) {
        client->type = NOTIFY_CLIENT_WEBSOCKET;
        if (notify_d_ws_upgrade(svc, client, buffer) == 0) {
            svc->client_count++;
        } else {
            client->active = 0;
            airy_mtx_unlock(&svc->lock);
            airy_sock_close(client_fd);
            return;
        }
    } else {
        client->type = NOTIFY_CLIENT_SOCKET;

        const char *topic = "inbound";
        const char *topic_hdr = "X-Topic: ";
        const char *th = strstr(buffer, topic_hdr);
        if (th) {
            const char *the = strstr(th + strlen(topic_hdr), "\r\n");
            if (the) {
                size_t tlen = (size_t)(the - (th + strlen(topic_hdr)));
                char *tn = (char *)AIRY_MALLOC(tlen + 1);
                if (tn) {
                    AIRY_MEMCPY(tn, th + strlen(topic_hdr), tlen);
                    tn[tlen] = '\0';
                    topic = tn;
                }
            }
        }
        client->topic = AIRY_STRDUP(topic);
        if (strcmp(topic, "inbound") != 0)
            AIRY_FREE((void *)topic);
        svc->client_count++;

        int ret = notify_d_enqueue(svc, buffer, client->topic, NULL);
        if (ret == 0) {
            svc->notified_count++;
        } else {
            svc->error_count++;
        }
    }

    uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
    size_t active_clients = 0;
    for (size_t i = 0; i < svc->client_count; i++) {
        if (svc->clients[i].active)
            active_clients++;
    }

    size_t depth = svc->pending_count;
    airy_mtx_unlock(&svc->lock);

    if (client->type == NOTIFY_CLIENT_SOCKET) {
        char response[4096];
        snprintf(response, sizeof(response),
                 "{"
                 "\"service\":\"notify_d\","
                 "\"status\":\"%s\","
                 "\"queued\":%llu,"
                 "\"pending\":%zu,"
                 "\"active_clients\":%zu,"
                 "\"uptime_sec\":%llu,"
                 "\"healthy\":%s"
                 "}",
                 svc->error_count > svc->notified_count / 2 ? "degraded" : "ok",
                 (unsigned long long)svc->notified_count, depth, active_clients,
                 (unsigned long long)uptime, notify_d_healthcheck(svc) ? "true" : "false");

        airy_sock_send(client_fd, response, strlen(response));
        airy_sock_close(client_fd);

        airy_mtx_lock(&svc->lock);
        client->active = 0;
        client->fd = AIRY_INVALID_SOCKET;
        airy_mtx_unlock(&svc->lock);
    }
}
