// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file notify_service.c
 * @brief Notification service core implementation (notify.* namespace).
 *
 * Provides the topic subscription registry (subscribe/unsubscribe), ring
 * event queue (enqueue), multi-protocol broadcast engine (broadcast_event)
 * and JSON-RPC method dispatch (dispatch_jsonrpc). Method names carry no
 * namespace prefix - the gateway strips the <ns>. prefix when forwarding
 * (02-l2-service-protocol.md §5).
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
                 "\"topic\":\"%s\","
                 "\"message\":\"%s\","
                 "\"timestamp\":%llu"
                 "}",
                 event->event_type ? event->event_type : "message",
                 event->topic ? event->topic : "default", event->message ? event->message : "",
                 (unsigned long long)event->timestamp);

    size_t broadcast_count = 0;

    for (size_t i = 0; i < svc->client_count; i++) {
        notify_client_t *client = &svc->clients[i];
        if (!client->active)
            continue;

        int subscribed = !event->topic || !client->topic ||
                         strcmp(event->topic, "broadcast") == 0 ||
                         strcmp(client->topic, event->topic) == 0;

        if (!subscribed && event->topic && client->client_id)
            subscribed = notify_d_has_subscription(svc, event->topic, client->client_id);

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

int notify_d_enqueue(notify_d_service_t *svc, const char *msg, const char *topic,
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
    event->topic = topic ? AIRY_STRDUP(topic) : AIRY_STRDUP("default");
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
            AIRY_FREE(sub->topic);
            sub->client_id = NULL;
            sub->topic = NULL;
            sub->active = 0;
        }
    }
    svc->subscription_count = 0;

    for (size_t i = 0; i < svc->pending_count; i++) {
        size_t idx = (svc->pending_head + i) % NOTIFY_D_MAX_PENDING;
        notify_event_t *event = svc->pending[idx];
        if (event) {
            AIRY_FREE(event->message);
            AIRY_FREE(event->topic);
            AIRY_FREE(event->event_type);
            AIRY_FREE(event);
        }
        svc->pending[idx] = NULL;
    }
    svc->pending_count = 0;
    svc->pending_head = 0;
    svc->pending_tail = 0;

    for (size_t i = 0; i < svc->client_count; i++) {
        AIRY_FREE(svc->clients[i].topic);
        AIRY_FREE(svc->clients[i].client_id);
        svc->clients[i].topic = NULL;
        svc->clients[i].client_id = NULL;
    }
    svc->client_count = 0;

    airy_mtx_destroy(&svc->lock);
}

static notify_subscription_t *notify_d_find_subscription(notify_d_service_t *svc,
                                                         const char *topic, const char *client_id)
{
    for (size_t i = 0; i < svc->subscription_count; i++) {
        notify_subscription_t *sub = &svc->subscriptions[i];
        if (!sub->active)
            continue;
        if (strcmp(sub->topic, topic) == 0 && strcmp(sub->client_id, client_id) == 0)
            return sub;
    }
    return NULL;
}

int notify_d_subscribe(notify_d_service_t *svc, const char *topic, const char *client_id)
{
    if (!svc || !topic || !client_id || !topic[0] || !client_id[0]) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "topic and client_id are required");
    }

    airy_mtx_lock(&svc->lock);

    if (notify_d_find_subscription(svc, topic, client_id)) {
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
    slot->topic = AIRY_STRDUP(topic);
    if (!slot->client_id || !slot->topic) {
        AIRY_FREE(slot->client_id);
        AIRY_FREE(slot->topic);
        slot->client_id = NULL;
        slot->topic = NULL;
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

int notify_d_unsubscribe(notify_d_service_t *svc, const char *topic, const char *client_id)
{
    if (!svc || !topic || !client_id || !topic[0] || !client_id[0]) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "topic and client_id are required");
    }

    airy_mtx_lock(&svc->lock);
    notify_subscription_t *sub = notify_d_find_subscription(svc, topic, client_id);
    if (sub) {
        AIRY_FREE(sub->client_id);
        AIRY_FREE(sub->topic);
        sub->client_id = NULL;
        sub->topic = NULL;
        sub->active = 0;
    }
    airy_mtx_unlock(&svc->lock);

    return AIRY_SUCCESS;
}

int notify_d_has_subscription(notify_d_service_t *svc, const char *topic, const char *client_id)
{
    if (!svc || !topic || !client_id)
        return 0;

    airy_mtx_lock(&svc->lock);
    int found = notify_d_find_subscription(svc, topic, client_id) != NULL;
    airy_mtx_unlock(&svc->lock);
    return found;
}

size_t notify_d_subscription_count(notify_d_service_t *svc, const char *topic)
{
    if (!svc || !topic)
        return 0;

    airy_mtx_lock(&svc->lock);
    size_t count = 0;
    for (size_t i = 0; i < svc->subscription_count; i++) {
        notify_subscription_t *sub = &svc->subscriptions[i];
        if (sub->active && strcmp(sub->topic, topic) == 0)
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
        const char *topic = jsonrpc_get_string_param(params, "topic", "default");
        const char *message = jsonrpc_get_string_param(params, "message", NULL);
        if (!message || !message[0])
            message = jsonrpc_get_string_param(params, "payload", NULL);
        const char *event_type = jsonrpc_get_string_param(params, "event", "message");

        if (!message || !message[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "message (or payload) is required",
                                      rid);
        } else {

            airy_mtx_lock(&svc->lock);
            int rc = notify_d_enqueue(svc, message, topic, event_type);
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
                cJSON_AddStringToObject(result, "topic", topic);
                cJSON_AddStringToObject(result, "event", event_type);
                cJSON_AddNumberToObject(result, "pending", (double)pending);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, topic));
            }
        }
    } else if (strcmp(method, "subscribe") == 0) {

        cJSON *params = cJSON_GetObjectItem(req, "params");
        const char *topic = jsonrpc_get_string_param(params, "topic", NULL);
        const char *client_id = jsonrpc_get_string_param(params, "client_id", NULL);

        if (!topic || !topic[0] || !client_id || !client_id[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "topic and client_id are required",
                                      rid);
        } else {
            int rc = notify_d_subscribe(svc, topic, client_id);
            if (rc != AIRY_SUCCESS) {
                out = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "failed to subscribe", rid);
            } else {
                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "status", "subscribed");
                cJSON_AddStringToObject(result, "topic", topic);
                cJSON_AddStringToObject(result, "client_id", client_id);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, topic));
            }
        }
    } else if (strcmp(method, "unsubscribe") == 0) {

        cJSON *params = cJSON_GetObjectItem(req, "params");
        const char *topic = jsonrpc_get_string_param(params, "topic", NULL);
        const char *client_id = jsonrpc_get_string_param(params, "client_id", NULL);

        if (!topic || !topic[0] || !client_id || !client_id[0]) {
            out = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "topic and client_id are required",
                                      rid);
        } else {
            int rc = notify_d_unsubscribe(svc, topic, client_id);
            if (rc != AIRY_SUCCESS) {
                out = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "failed to unsubscribe", rid);
            } else {
                result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "status", "unsubscribed");
                cJSON_AddStringToObject(result, "topic", topic);
                cJSON_AddStringToObject(result, "client_id", client_id);
                cJSON_AddNumberToObject(result, "subscribers",
                                        (double)notify_d_subscription_count(svc, topic));
            }
        }
    } else if (strcmp(method, "list") == 0) {

        const char *topic_names[NOTIFY_D_MAX_SUBSCRIPTIONS + NOTIFY_D_MAX_CLIENTS];
        size_t topic_count = 0;
        size_t active_clients = 0;
        size_t total_subs = 0;

        airy_mtx_lock(&svc->lock);
        for (size_t i = 0; i < svc->client_count; i++) {
            if (!svc->clients[i].active)
                continue;
            active_clients++;
            const char *tp = svc->clients[i].topic;
            if (tp && tp[0]) {
                int known = 0;
                for (size_t k = 0; k < topic_count; k++) {
                    if (strcmp(topic_names[k], tp) == 0) {
                        known = 1;
                        break;
                    }
                }
                if (!known && topic_count < (sizeof(topic_names) / sizeof(topic_names[0])))
                    topic_names[topic_count++] = tp;
            }
        }
        for (size_t i = 0; i < svc->subscription_count; i++) {
            notify_subscription_t *sub = &svc->subscriptions[i];
            if (!sub->active)
                continue;
            total_subs++;
            int known = 0;
            for (size_t k = 0; k < topic_count; k++) {
                if (strcmp(topic_names[k], sub->topic) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known && topic_count < (sizeof(topic_names) / sizeof(topic_names[0])))
                topic_names[topic_count++] = sub->topic;
        }
        airy_mtx_unlock(&svc->lock);

        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "clients", (double)active_clients);
        cJSON_AddNumberToObject(result, "subscriptions", (double)total_subs);
        cJSON *topics_arr = cJSON_CreateArray();
        for (size_t k = 0; k < topic_count; k++) {
            size_t subs = notify_d_subscription_count(svc, topic_names[k]);
            size_t act = 0;
            airy_mtx_lock(&svc->lock);
            for (size_t i = 0; i < svc->client_count; i++) {
                if (svc->clients[i].active && svc->clients[i].topic &&
                    strcmp(svc->clients[i].topic, topic_names[k]) == 0)
                    act++;
            }
            airy_mtx_unlock(&svc->lock);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "topic", topic_names[k]);
            cJSON_AddNumberToObject(item, "subscribers", (double)subs);
            cJSON_AddNumberToObject(item, "active_clients", (double)act);
            cJSON_AddItemToArray(topics_arr, item);
        }
        cJSON_AddItemToObject(result, "topics", topics_arr);
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
