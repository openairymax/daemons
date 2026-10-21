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

void notify_d_sse_heartbeat(notify_d_service_t *svc)
{
    if (!svc)
        return;

    /* SSE 注释帧（":" 开头）按规范被客户端解析器忽略，仅维持空闲计数
     * 复位（gateway epoch watch 侧 recv 到数据即清 idle）；调用方持锁，
     * 9 字节短帧发送与 accept 路径持锁发 SSE 握手头同为既有模式。 */
    for (size_t i = 0; i < svc->client_count; i++) {
        notify_client_t *client = &svc->clients[i];
        if (!client->active || client->type != NOTIFY_CLIENT_SSE ||
            !client->handshake_done)
            continue;
        if (airy_sock_send(client->fd, ": ping\n\n", 8) > 0)
            client->messages_sent++;
    }
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

void notify_d_drain(notify_d_service_t *svc)
{
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

    notify_d_drain(svc);

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

typedef struct {
    notify_d_service_t *svc;
    cJSON *req;
    int rid;
    char *err;
    cJSON *result;
} ntf_call_t;

typedef void (*ntf_fn_t)(ntf_call_t *c);

typedef struct {
    const char *name;
    ntf_fn_t fn;
    int status;
} ntf_method_t;

typedef struct {
    const char *names[NOTIFY_D_MAX_SUBSCRIPTIONS + NOTIFY_D_MAX_CLIENTS];
    size_t count;
} ntf_topics_t;

static void ntf_publish(ntf_call_t *c)
{
    cJSON *params = cJSON_GetObjectItem(c->req, "params");
    const char *topic = jsonrpc_get_string_param(params, "topic", "default");
    const char *message = jsonrpc_get_string_param(params, "message", NULL);
    if (!message || !message[0])
        message = jsonrpc_get_string_param(params, "payload", NULL);
    const char *event_type = jsonrpc_get_string_param(params, "event", "message");

    if (!message || !message[0]) {
        c->err = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "message (or payload) is required",
                                     c->rid);
        return;
    }

    airy_mtx_lock(&c->svc->lock);
    int rc = notify_d_enqueue(c->svc, message, topic, event_type);
    size_t pending = c->svc->pending_count;
    airy_mtx_unlock(&c->svc->lock);
    if (rc != AIRY_SUCCESS) {
        c->err = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR,
                                     rc == AIRY_ERR_OUT_OF_MEMORY ? "out of memory"
                                                                  : "pending queue full",
                                     c->rid);
        return;
    }

    c->result = cJSON_CreateObject();
    cJSON_AddBoolToObject(c->result, "queued", 1);
    cJSON_AddStringToObject(c->result, "topic", topic);
    cJSON_AddStringToObject(c->result, "event", event_type);
    cJSON_AddNumberToObject(c->result, "pending", (double)pending);
    cJSON_AddNumberToObject(c->result, "subscribers",
                            (double)notify_d_subscription_count(c->svc, topic));
}

static void ntf_member(ntf_call_t *c)
{
    cJSON *params = cJSON_GetObjectItem(c->req, "params");
    const char *topic = jsonrpc_get_string_param(params, "topic", NULL);
    const char *client_id = jsonrpc_get_string_param(params, "client_id", NULL);
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(c->req, "method"));
    int on = method && strcmp(method, "subscribe") == 0;

    if (!topic || !topic[0] || !client_id || !client_id[0]) {
        c->err = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "topic and client_id are required",
                                     c->rid);
        return;
    }

    int rc = on ? notify_d_subscribe(c->svc, topic, client_id)
                : notify_d_unsubscribe(c->svc, topic, client_id);
    if (rc != AIRY_SUCCESS) {
        c->err = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR,
                                     on ? "failed to subscribe" : "failed to unsubscribe", c->rid);
        return;
    }

    c->result = cJSON_CreateObject();
    cJSON_AddStringToObject(c->result, "status", on ? "subscribed" : "unsubscribed");
    cJSON_AddStringToObject(c->result, "topic", topic);
    cJSON_AddStringToObject(c->result, "client_id", client_id);
    cJSON_AddNumberToObject(c->result, "subscribers",
                            (double)notify_d_subscription_count(c->svc, topic));
}

static void ntf_take(ntf_topics_t *t, const char *topic)
{
    if (!topic || !topic[0])
        return;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->names[i], topic) == 0)
            return;
    }
    if (t->count < sizeof(t->names) / sizeof(t->names[0]))
        t->names[t->count++] = topic;
}

static void ntf_list(ntf_call_t *c)
{
    ntf_topics_t topics;
    size_t clients = 0;
    size_t subs = 0;

    topics.count = 0;
    airy_mtx_lock(&c->svc->lock);
    for (size_t i = 0; i < c->svc->client_count; i++) {
        if (!c->svc->clients[i].active)
            continue;
        clients++;
        ntf_take(&topics, c->svc->clients[i].topic);
    }
    for (size_t i = 0; i < c->svc->subscription_count; i++) {
        if (!c->svc->subscriptions[i].active)
            continue;
        subs++;
        ntf_take(&topics, c->svc->subscriptions[i].topic);
    }
    airy_mtx_unlock(&c->svc->lock);

    c->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(c->result, "clients", (double)clients);
    cJSON_AddNumberToObject(c->result, "subscriptions", (double)subs);

    cJSON *arr = cJSON_CreateArray();
    for (size_t k = 0; k < topics.count; k++) {
        size_t active = 0;
        airy_mtx_lock(&c->svc->lock);
        for (size_t i = 0; i < c->svc->client_count; i++) {
            if (c->svc->clients[i].active && c->svc->clients[i].topic &&
                strcmp(c->svc->clients[i].topic, topics.names[k]) == 0)
                active++;
        }
        airy_mtx_unlock(&c->svc->lock);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "topic", topics.names[k]);
        cJSON_AddNumberToObject(item, "subscribers",
                                (double)notify_d_subscription_count(c->svc, topics.names[k]));
        cJSON_AddNumberToObject(item, "active_clients", (double)active);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(c->result, "topics", arr);
}

static void ntf_health(ntf_call_t *c)
{
    notify_d_service_t *svc = c->svc;
    size_t act = 0;
    size_t subs = 0;

    airy_mtx_lock(&svc->lock);
    size_t pending = svc->pending_count;
    int running = svc->event_running ? 1 : 0;
    uint64_t notified = svc->notified_count;
    uint64_t errors = svc->error_count;
    for (size_t i = 0; i < svc->client_count; i++)
        act += svc->clients[i].active ? 1 : 0;
    for (size_t i = 0; i < svc->subscription_count; i++)
        subs += svc->subscriptions[i].active ? 1 : 0;
    airy_mtx_unlock(&svc->lock);

    uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
    double occupancy = (double)pending / (double)NOTIFY_D_MAX_PENDING;
    const char *status = (!running || pending >= NOTIFY_D_MAX_PENDING) ? "degraded" : "ok";

    c->result = cJSON_CreateObject();
    cJSON_AddStringToObject(c->result, "status", status);
    cJSON_AddStringToObject(c->result, "service", "notify_d");
    cJSON_AddNumberToObject(c->result, "queue_pending", (double)pending);
    cJSON_AddNumberToObject(c->result, "queue_capacity", (double)NOTIFY_D_MAX_PENDING);
    cJSON_AddNumberToObject(c->result, "queue_occupancy", occupancy);
    cJSON_AddBoolToObject(c->result, "consumer_running", running);
    cJSON_AddNumberToObject(c->result, "active_clients", (double)act);
    cJSON_AddNumberToObject(c->result, "subscriptions", (double)subs);
    cJSON_AddNumberToObject(c->result, "notified", (double)notified);
    cJSON_AddNumberToObject(c->result, "errors", (double)errors);
    cJSON_AddNumberToObject(c->result, "uptime_s", (double)uptime);
}

static void ntf_shutdown(ntf_call_t *c)
{
    c->result = cJSON_CreateObject();
    cJSON_AddStringToObject(c->result, "status", "shutting_down");
}

static void ntf_stats(ntf_call_t *c)
{
    notify_d_service_t *svc = c->svc;

    airy_mtx_lock(&svc->lock);
    size_t pending = svc->pending_count;
    size_t clients = svc->client_count;
    uint64_t notified = svc->notified_count;
    uint64_t errors = svc->error_count;
    airy_mtx_unlock(&svc->lock);

    uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
    c->result = cJSON_CreateObject();
    cJSON_AddStringToObject(c->result, "daemon", "notify_d");
    cJSON_AddNumberToObject(c->result, "uptime_s", (double)uptime);
    cJSON_AddNumberToObject(c->result, "notified", (double)notified);
    cJSON_AddNumberToObject(c->result, "errors", (double)errors);
    cJSON_AddNumberToObject(c->result, "clients", (double)clients);
    cJSON_AddNumberToObject(c->result, "pending", (double)pending);
}

static void ntf_ping(ntf_call_t *c)
{
    c->result = cJSON_CreateObject();
    cJSON_AddStringToObject(c->result, "status", "ok");
    cJSON_AddStringToObject(c->result, "service", "notify_d");
    cJSON_AddNumberToObject(c->result, "uptime_s",
                            (double)((uint64_t)time(NULL) - c->svc->start_time));
    cJSON_AddNumberToObject(c->result, "timestamp", (double)time(NULL) * 1000.0);
}

static const ntf_method_t ntf_methods[] = {
    {"publish", ntf_publish, NOTIFY_D_METHOD_HANDLED},
    {"subscribe", ntf_member, NOTIFY_D_METHOD_HANDLED},
    {"unsubscribe", ntf_member, NOTIFY_D_METHOD_HANDLED},
    {"list", ntf_list, NOTIFY_D_METHOD_HANDLED},
    {"health", ntf_health, NOTIFY_D_METHOD_HANDLED},
    {"shutdown", ntf_shutdown, NOTIFY_D_METHOD_SHUTDOWN},
    {"get_stats", ntf_stats, NOTIFY_D_METHOD_HANDLED},
    {"health_check", ntf_ping, NOTIFY_D_METHOD_HANDLED},
};

static const ntf_method_t *ntf_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof(ntf_methods) / sizeof(ntf_methods[0]); i++)
        if (strcmp(ntf_methods[i].name, name) == 0)
            return &ntf_methods[i];
    return NULL;
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

    const ntf_method_t *hit = ntf_lookup(m->valuestring);
    if (!hit) {
        cJSON_Delete(req);
        return NOTIFY_D_METHOD_NOT_RPC;
    }

    ntf_call_t call = {.svc = svc,
                       .req = req,
                       .rid = cJSON_IsNumber(idj) ? idj->valueint : 0};
    hit->fn(&call);

    /* jsonrpc_build_success 在全部路径上接管 result 所有权，此处不得再释放。 */
    if (!call.err && call.result) {
        call.err = jsonrpc_build_success(call.result, call.rid);
        if (!call.err)
            call.err = jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "internal error", call.rid);
    }
    if (call.err) {
        snprintf(response, response_size, "%s", call.err);
        AIRY_FREE(call.err);
    }

    cJSON_Delete(req);
    return hit->status;
}
