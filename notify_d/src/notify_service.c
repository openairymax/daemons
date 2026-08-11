// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file notify_service.c
 * @brief 通知服务核心实现（notify.* 命名空间）
 *
 * 提供频道订阅注册表（subscribe/unsubscribe）、环形事件队列（enqueue）、
 * 多协议广播引擎（broadcast_event）与 JSON-RPC 方法分发（dispatch_jsonrpc）。
 * 方法名不带命名空间前缀——gateway 转发时已剥离 <ns>. 前缀（02-l2-service-protocol.md §5）。
 *
 */

#include "notify_service.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "jsonrpc_helpers.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int notify_d_send_ws_frame(notify_client_t *client, const char *payload, size_t payload_len)
{
    if (!client || !payload || client->fd == AIRY_INVALID_SOCKET) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter or invalid socket");
    }

    unsigned char frame[10];
    size_t header_len = 2;
    frame[0] = 0x81;

    if (payload_len <= 125) {
        frame[1] = (unsigned char)payload_len;
    } else if (payload_len <= 65535) {
        frame[1] = 126;
        frame[2] = (unsigned char)(payload_len >> 8);
        frame[3] = (unsigned char)(payload_len & 0xFF);
        header_len = 4;
    } else {
        frame[1] = 127;
        for (int i = 0; i < 8; i++)
            frame[2 + i] = (unsigned char)(payload_len >> (56 - 8 * i));
        header_len = 10;
    }

    if (airy_sock_send(client->fd, (const char *)frame, header_len) <= 0) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to send WS frame header");
    }
    if (airy_sock_send(client->fd, payload, payload_len) <= 0) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to send WS frame payload");
    }

    return 0;
}

int notify_d_broadcast_event(notify_d_service_t *svc, const notify_event_t *event)
{
    if (!svc || !event) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char json_msg[8192];
    int msg_len =
        snprintf(json_msg, sizeof(json_msg),
                 "{"
                 "\"event\":\"%s\","
                 "\"channel\":\"%s\","
                 "\"message\":\"%s\","
                 "\"timestamp\":%llu"
                 "}",
                 event->event_type ? event->event_type : "message",
                 event->channel ? event->channel : "default", event->message ? event->message : "",
                 (unsigned long long)event->timestamp);

    size_t broadcast_count = 0;

    for (size_t i = 0; i < svc->client_count; i++) {
        notify_client_t *client = &svc->clients[i];
        if (!client->active)
            continue;

        int subscribed = !event->channel || !client->channel ||
                         strcmp(event->channel, "broadcast") == 0 ||
                         strcmp(client->channel, event->channel) == 0;

        if (!subscribed && event->channel && client->client_id)
            subscribed = notify_d_has_subscription(svc, event->channel, client->client_id);

        if (!subscribed)
            continue;

        if (client->type == NOTIFY_CLIENT_WEBSOCKET && client->handshake_done) {
            notify_d_send_ws_frame(client, json_msg, (size_t)msg_len);
            client->messages_sent++;
            broadcast_count++;
        } else if (client->type == NOTIFY_CLIENT_SOCKET) {
            airy_sock_send(client->fd, json_msg, (size_t)msg_len);
            client->messages_sent++;
            broadcast_count++;
        } else if (client->type == NOTIFY_CLIENT_SSE) {
            char sse_msg[8448];
            int sse_len = snprintf(sse_msg, sizeof(sse_msg), "event: %s\ndata: %s\n\n",
                                   event->event_type ? event->event_type : "message", json_msg);
            airy_sock_send(client->fd, sse_msg, (size_t)sse_len);
            client->messages_sent++;
            broadcast_count++;
        }
    }

    return (int)broadcast_count;
}

int notify_d_enqueue(notify_d_service_t *svc, const char *msg, const char *channel,
                     const char *event_type)
{
    if (!svc || !msg) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    if (svc->pending_count >= NOTIFY_D_MAX_PENDING) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "pending queue full");
    }

    notify_event_t *event = (notify_event_t *)AIRY_CALLOC(1, sizeof(notify_event_t));
    if (!event) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "calloc failed for notify_event_t");
    }

    event->message = AIRY_STRDUP(msg);
    event->channel = channel ? AIRY_STRDUP(channel) : AIRY_STRDUP("default");
    event->event_type = event_type ? AIRY_STRDUP(event_type) : AIRY_STRDUP("message");
    event->timestamp = (uint64_t)time(NULL);

    svc->pending[svc->pending_tail] = event;
    svc->pending_tail = (svc->pending_tail + 1) % NOTIFY_D_MAX_PENDING;
    svc->pending_count++;

    return 0;
}

int notify_d_service_init(notify_d_service_t *svc)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "svc is NULL");
    }

    __builtin_memset(svc, 0, sizeof(*svc));
    airy_mtx_init(&svc->lock);
    svc->start_time = (uint64_t)time(NULL);
    return AIRY_SUCCESS;
}

void notify_d_service_destroy(notify_d_service_t *svc)
{
    if (!svc)
        return;

    for (size_t i = 0; i < svc->subscription_count; i++) {
        notify_subscription_t *sub = &svc->subscriptions[i];
        if (sub->active) {
            AIRY_FREE(sub->client_id);
            AIRY_FREE(sub->channel);
            sub->client_id = NULL;
            sub->channel = NULL;
            sub->active = 0;
        }
    }
    svc->subscription_count = 0;

    for (size_t i = 0; i < svc->pending_count; i++) {
        size_t idx = (svc->pending_head + i) % NOTIFY_D_MAX_PENDING;
        notify_event_t *event = svc->pending[idx];
        if (event) {
            AIRY_FREE(event->message);
            AIRY_FREE(event->channel);
            AIRY_FREE(event->event_type);
            AIRY_FREE(event);
        }
        svc->pending[idx] = NULL;
    }
    svc->pending_count = 0;
    svc->pending_head = 0;
    svc->pending_tail = 0;

    for (size_t i = 0; i < svc->client_count; i++) {
        AIRY_FREE(svc->clients[i].channel);
        AIRY_FREE(svc->clients[i].client_id);
        svc->clients[i].channel = NULL;
        svc->clients[i].client_id = NULL;
    }
    svc->client_count = 0;

    airy_mtx_destroy(&svc->lock);
}

static notify_subscription_t *notify_d_find_subscription(notify_d_service_t *svc,
                                                         const char *channel, const char *client_id)
{
    for (size_t i = 0; i < svc->subscription_count; i++) {
        notify_subscription_t *sub = &svc->subscriptions[i];
        if (!sub->active)
            continue;
        if (strcmp(sub->channel, channel) == 0 && strcmp(sub->client_id, client_id) == 0)
            return sub;
    }
    return NULL;
}

int notify_d_subscribe(notify_d_service_t *svc, const char *channel, const char *client_id)
{
    if (!svc || !channel || !client_id || !channel[0] || !client_id[0]) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "channel and client_id are required");
    }

    airy_mtx_lock(&svc->lock);

    if (notify_d_find_subscription(svc, channel, client_id)) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_SUCCESS;
    }

    notify_subscription_t *slot = NULL;
    for (size_t i = 0; i < svc->subscription_count; i++) {
        if (!svc->subscriptions[i].active) {
            slot = &svc->subscriptions[i];
            break;
        }
    }
    if (!slot && svc->subscription_count < NOTIFY_D_MAX_SUBSCRIPTIONS)
        slot = &svc->subscriptions[svc->subscription_count];

    if (!slot) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "subscription registry full");
    }

    slot->client_id = AIRY_STRDUP(client_id);
    slot->channel = AIRY_STRDUP(channel);
    if (!slot->client_id || !slot->channel) {
        AIRY_FREE(slot->client_id);
        AIRY_FREE(slot->channel);
        slot->client_id = NULL;
        slot->channel = NULL;
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "strdup failed for subscription");
    }
    slot->active = 1;

    size_t idx = (size_t)(slot - svc->subscriptions);
    if (idx >= svc->subscription_count)
        svc->subscription_count = idx + 1;

    airy_mtx_unlock(&svc->lock);
    return AIRY_SUCCESS;
}

int notify_d_unsubscribe(notify_d_service_t *svc, const char *channel, const char *client_id)
{
    if (!svc || !channel || !client_id || !channel[0] || !client_id[0]) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "channel and client_id are required");
    }

    airy_mtx_lock(&svc->lock);
    notify_subscription_t *sub = notify_d_find_subscription(svc, channel, client_id);
    if (sub) {
        AIRY_FREE(sub->client_id);
        AIRY_FREE(sub->channel);
        sub->client_id = NULL;
        sub->channel = NULL;
        sub->active = 0;
    }
    airy_mtx_unlock(&svc->lock);

    return AIRY_SUCCESS;
}

int notify_d_has_subscription(notify_d_service_t *svc, const char *channel, const char *client_id)
{
    if (!svc || !channel || !client_id)
        return 0;

    airy_mtx_lock(&svc->lock);
    int found = notify_d_find_subscription(svc, channel, client_id) != NULL;
    airy_mtx_unlock(&svc->lock);
    return found;
}

size_t notify_d_subscription_count(notify_d_service_t *svc, const char *channel)
{
    if (!svc || !channel)
        return 0;

    airy_mtx_lock(&svc->lock);
    size_t count = 0;
    for (size_t i = 0; i < svc->subscription_count; i++) {
        notify_subscription_t *sub = &svc->subscriptions[i];
        if (sub->active && strcmp(sub->channel, channel) == 0)
            count++;
    }
    airy_mtx_unlock(&svc->lock);
    return count;
}

size_t notify_d_active_client_count(notify_d_service_t *svc)
{
    if (!svc)
        return 0;

    airy_mtx_lock(&svc->lock);
    size_t count = 0;
    for (size_t i = 0; i < svc->client_count; i++) {
        if (svc->clients[i].active)
            count++;
    }
    airy_mtx_unlock(&svc->lock);
    return count;
}

int notify_d_dispatch_jsonrpc(notify_d_service_t *svc, const char *request, char *response,
                              size_t response_size)
{
    if (!svc || !request || !response || response_size == 0)
        return NOTIFY_D_METHOD_NOT_RPC;

    cJSON *req = cJSON_Parse(request);
    if (!req)
        return NOTIFY_D_METHOD_NOT_RPC;

    cJSON *m = cJSON_GetObjectItem(req, "method");
    cJSON *idj = cJSON_GetObjectItem(req, "id");
    if (!cJSON_IsString(m) || !idj) {
        cJSON_Delete(req);
        return NOTIFY_D_METHOD_NOT_RPC;
    }
    int rid = cJSON_IsNumber(idj) ? idj->valueint : 0;
    const char *method = m->valuestring;

    cJSON *result = NULL;
    char *out = NULL;
    int status = NOTIFY_D_METHOD_HANDLED;

    if (strcmp(method, "publish") == 0) {

        cJSON *params = cJSON_GetObjectItem(req, "params");
        const char *channel = jsonrpc_get_string_param(params, "channel", "default");
        const char *message = jsonrpc_get_string_param(params, "message", NULL);
        if (!message || !message[0])
            message = jsonrpc_get_string_param(params, "payload", NULL);
        const char *event_type = jsonrpc_get_string_param(params, "event", "message");

        if (!message || !message[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "message (or payload) is required",
                                      rid);
        } else {

            airy_mtx_lock(&svc->lock);
            int rc = notify_d_enqueue(svc, message, channel, event_type);
            airy_mtx_unlock(&svc->lock);
            if (rc != AIRY_SUCCESS) {
                out = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR,
                                          rc == AIRY_ERR_OUT_OF_MEMORY ? "out of memory" :
                                                                         "pending queue full",
                                          rid);
            } else {
                airy_mtx_lock(&svc->lock);
                size_t pending = svc->pending_count;
                airy_mtx_unlock(&svc->lock);
                result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "queued", 1);
                cJSON_AddStringToObject(result, "channel", channel);
                cJSON_AddStringToObject(result, "event", event_type);
                cJSON_AddNumberToObject(result, "pending", (double)pending);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, channel));
            }
        }
    } else if (strcmp(method, "subscribe") == 0) {

        cJSON *params = cJSON_GetObjectItem(req, "params");
        const char *channel = jsonrpc_get_string_param(params, "channel", NULL);
        const char *client_id = jsonrpc_get_string_param(params, "client_id", NULL);

        if (!channel || !channel[0] || !client_id || !client_id[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "channel and client_id are required",
                                      rid);
        } else {
            int rc = notify_d_subscribe(svc, channel, client_id);
            if (rc != AIRY_SUCCESS) {
                out = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "failed to subscribe", rid);
            } else {
                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "status", "subscribed");
                cJSON_AddStringToObject(result, "channel", channel);
                cJSON_AddStringToObject(result, "client_id", client_id);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, channel));
            }
        }
    } else if (strcmp(method, "unsubscribe") == 0) {

        cJSON *params = cJSON_GetObjectItem(req, "params");
        const char *channel = jsonrpc_get_string_param(params, "channel", NULL);
        const char *client_id = jsonrpc_get_string_param(params, "client_id", NULL);

        if (!channel || !channel[0] || !client_id || !client_id[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "channel and client_id are required",
                                      rid);
        } else {
            int rc = notify_d_unsubscribe(svc, channel, client_id);
            if (rc != AIRY_SUCCESS) {
                out = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "failed to unsubscribe", rid);
            } else {
                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "status", "unsubscribed");
                cJSON_AddStringToObject(result, "channel", channel);
                cJSON_AddStringToObject(result, "client_id", client_id);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, channel));
            }
        }
    } else if (strcmp(method, "list") == 0) {

        const char *chan_names[NOTIFY_D_MAX_SUBSCRIPTIONS + NOTIFY_D_MAX_CLIENTS];
        size_t chan_count = 0;
        size_t active_clients = 0;
        size_t total_subs = 0;

        airy_mtx_lock(&svc->lock);
        for (size_t i = 0; i < svc->client_count; i++) {
            if (!svc->clients[i].active)
                continue;
            active_clients++;
            const char *ch = svc->clients[i].channel;
            if (ch && ch[0]) {
                int known = 0;
                for (size_t k = 0; k < chan_count; k++) {
                    if (strcmp(chan_names[k], ch) == 0) {
                        known = 1;
                        break;
                    }
                }
                if (!known && chan_count < (sizeof(chan_names) / sizeof(chan_names[0])))
                    chan_names[chan_count++] = ch;
            }
        }
        for (size_t i = 0; i < svc->subscription_count; i++) {
            notify_subscription_t *sub = &svc->subscriptions[i];
            if (!sub->active)
                continue;
            total_subs++;
            int known = 0;
            for (size_t k = 0; k < chan_count; k++) {
                if (strcmp(chan_names[k], sub->channel) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known && chan_count < (sizeof(chan_names) / sizeof(chan_names[0])))
                chan_names[chan_count++] = sub->channel;
        }
        airy_mtx_unlock(&svc->lock);

        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "clients", (double)active_clients);
        cJSON_AddNumberToObject(result, "subscriptions", (double)total_subs);
        cJSON *channels_arr = cJSON_CreateArray();
        for (size_t k = 0; k < chan_count; k++) {
            size_t subs = notify_d_subscription_count(svc, chan_names[k]);
            size_t act = 0;
            airy_mtx_lock(&svc->lock);
            for (size_t i = 0; i < svc->client_count; i++) {
                if (svc->clients[i].active && svc->clients[i].channel &&
                    strcmp(svc->clients[i].channel, chan_names[k]) == 0)
                    act++;
            }
            airy_mtx_unlock(&svc->lock);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "channel", chan_names[k]);
            cJSON_AddNumberToObject(item, "subscribers", (double)subs);
            cJSON_AddNumberToObject(item, "active_clients", (double)act);
            cJSON_AddItemToArray(channels_arr, item);
        }
        cJSON_AddItemToObject(result, "channels", channels_arr);
    } else if (strcmp(method, "health") == 0) {

        size_t pending = 0;
        size_t active_clients = 0;
        size_t total_subs = 0;
        int consumer_running = 0;
        uint64_t notified = 0;
        uint64_t errors = 0;

        airy_mtx_lock(&svc->lock);
        pending = svc->pending_count;
        active_clients = 0;
        for (size_t i = 0; i < svc->client_count; i++) {
            if (svc->clients[i].active)
                active_clients++;
        }
        for (size_t i = 0; i < svc->subscription_count; i++) {
            if (svc->subscriptions[i].active)
                total_subs++;
        }
        consumer_running = svc->event_running ? 1 : 0;
        notified = svc->notified_count;
        errors = svc->error_count;
        airy_mtx_unlock(&svc->lock);

        uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
        double occupancy =
            NOTIFY_D_MAX_PENDING > 0 ? (double)pending / (double)NOTIFY_D_MAX_PENDING : 0.0;
        const char *status =
            (!consumer_running || pending >= NOTIFY_D_MAX_PENDING) ? "degraded" : "ok";

        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", status);
        cJSON_AddStringToObject(result, "service", "notify_d");
        cJSON_AddNumberToObject(result, "queue_pending", (double)pending);
        cJSON_AddNumberToObject(result, "queue_capacity", (double)NOTIFY_D_MAX_PENDING);
        cJSON_AddNumberToObject(result, "queue_occupancy", occupancy);
        cJSON_AddBoolToObject(result, "consumer_running", consumer_running);
        cJSON_AddNumberToObject(result, "active_clients", (double)active_clients);
        cJSON_AddNumberToObject(result, "subscriptions", (double)total_subs);
        cJSON_AddNumberToObject(result, "notified", (double)notified);
        cJSON_AddNumberToObject(result, "errors", (double)errors);
        cJSON_AddNumberToObject(result, "uptime_s", (double)uptime);
    } else if (strcmp(method, "shutdown") == 0) {

        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "shutting_down");
        status = NOTIFY_D_METHOD_SHUTDOWN;
    } else if (strcmp(method, "get_stats") == 0) {
        uint64_t notified = 0;
        uint64_t errors = 0;
        size_t clients = 0;
        size_t pending = 0;
        airy_mtx_lock(&svc->lock);
        notified = svc->notified_count;
        errors = svc->error_count;
        clients = svc->client_count;
        pending = svc->pending_count;
        airy_mtx_unlock(&svc->lock);
        uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;

        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "daemon", "notify_d");
        cJSON_AddNumberToObject(result, "uptime_s", (double)uptime);
        cJSON_AddNumberToObject(result, "notified", (double)notified);
        cJSON_AddNumberToObject(result, "errors", (double)errors);
        cJSON_AddNumberToObject(result, "clients", (double)clients);
        cJSON_AddNumberToObject(result, "pending", (double)pending);
    } else if (strcmp(method, "health_check") == 0) {
        uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        cJSON_AddStringToObject(result, "service", "notify_d");
        cJSON_AddNumberToObject(result, "uptime_s", (double)uptime);
        cJSON_AddNumberToObject(result, "timestamp", (double)time(NULL) * 1000.0);
    } else {

        cJSON_Delete(req);
        return NOTIFY_D_METHOD_NOT_RPC;
    }

    if (out) {
        snprintf(response, response_size, "%s", out);
        AIRY_FREE(out);
    } else if (result) {
        char *built = jsonrpc_build_success(result, rid);
        if (built) {
            snprintf(response, response_size, "%s", built);
            AIRY_FREE(built);
        } else {

            snprintf(response, response_size,
                     "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"internal error\"},"
                     "\"id\":%d}",
                     JSONRPC_INTERNAL_ERROR, rid);
            status = NOTIFY_D_METHOD_HANDLED;
        }
    }

    cJSON_Delete(req);
    return status;
}
