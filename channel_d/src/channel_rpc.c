// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_rpc.c
 * @brief channel.* RPC 方法域：ping / list / open / close / send /
 *        health / health_check / get_stats。
 *
 * cJSON 参数直接交 chan_rpc_call 解析（无字符串回环），
 * 结果 JSON 包装为 JSON-RPC success 响应；参数校验失败（fail-closed：
 * 缺 id/data、非法类型）映射 Invalid params(-32602)，其余服务错误
 * 映射 Internal error(-32603)。服务单例句柄 g_svc 由 svc.c 持有。
 */

#include "airy_memory.h"
#include "channel_d_internal.h"
#include "error.h"
#include "svc_channel_d.h"

#include "daemon_main.h"

#include <stdio.h>
#include <string.h>

/* Extract channel_id from cJSON params: supports string and numeric ids,
 * returning an AIRY_MALLOC string or NULL. cJSON parsing fails closed on
 * malformed input (the original hand-written strstr/strchr parsing could
 * not handle numeric ids nor escaped quotes, silently dropping send data
 * while still reporting success). */
static char *channel_param_id_str(cJSON *params)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0])
        return AIRY_STRDUP(id->valuestring);
    if (cJSON_IsNumber(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", id->valueint);
        return AIRY_STRDUP(buf);
    }
    return NULL;
}

static int chan_rpc_call(const char *method, cJSON *params,
                                  char **response_json, void *user_data)
{
    channel_service_t *svc = (channel_service_t *)user_data;
    if (!svc || !method || !response_json) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    if (!params) {
        params = cJSON_CreateObject();
        if (!params)
            AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to create empty params");
    }

    if (strcmp(method, "ping") == 0) {
        char *id = channel_param_id_str(params);
        if (!id) {
            bool healthy = channel_service_is_healthy(svc);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"%s\"}", healthy ? "ok" : "degraded");
            *response_json = AIRY_STRDUP(buf);
            return 0;
        }
        int64_t latency_ms = 0;
        int rc = channel_service_ping(svc, id, &latency_ms);
        if (rc != CHANNEL_OK) {
            AIRY_FREE(id);
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"ping failed: %d\",\"latency_ms\":%lld}", rc,
                     (long long)latency_ms);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_ping failed");
        }
        size_t sz =
            snprintf(NULL, 0, "{\"status\":\"ok\",\"channel_id\":\"%s\",\"latency_ms\":%lld}", id,
                     (long long)latency_ms) +
            1;
        char *buf = (char *)AIRY_MALLOC(sz);
        if (!buf) {
            AIRY_FREE(id);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for ping response buffer");
        }
        snprintf(buf, sz, "{\"status\":\"ok\",\"channel_id\":\"%s\",\"latency_ms\":%lld}", id,
                 (long long)latency_ms);
        AIRY_FREE(id);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "list") == 0) {
        channel_info_t info_list[CHANNEL_MAX_CHANNELS];
        size_t count = 0;
        int rc = channel_service_list(svc, info_list, CHANNEL_MAX_CHANNELS, &count);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"list failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_list failed");
        }

        size_t buf_size = 4096 + count * 1024;
        char *buf = (char *)AIRY_MALLOC(buf_size);
        if (!buf) {
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for list response buffer");
        }

        size_t pos = 0;
        pos += snprintf(buf + pos, buf_size - pos, "{\"channels\":[");
        for (size_t i = 0; i < count; i++) {
            if (i > 0)
                pos += snprintf(buf + pos, buf_size - pos, ",");
            pos += snprintf(buf + pos, buf_size - pos,
                            "{\"id\":\"%s\",\"name\":\"%s\",\"type\":%d,\"status\":%d,\"sent\":%zu,"
                            "\"recv\":%zu}",
                            info_list[i].channel_id, info_list[i].name, info_list[i].type,
                            info_list[i].status, info_list[i].messages_sent,
                            info_list[i].messages_received);
        }
        pos += snprintf(buf + pos, buf_size - pos, "]}");
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "stats") == 0) {
        /* L2 standard method channel.get_stats (02-l2-service-protocol.md
         * §6.1): returns the channel count + cumulative sent/received message
         * counts per channel (real statistics). */
        channel_info_t info_list[CHANNEL_MAX_CHANNELS];
        size_t count = 0;
        int rc = channel_service_list(svc, info_list, CHANNEL_MAX_CHANNELS, &count);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"list failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_list failed in stats");
        }
        uint64_t sent = 0, recv = 0;
        for (size_t i = 0; i < count; i++) {
            sent += info_list[i].messages_sent;
            recv += info_list[i].messages_received;
        }
        size_t sz = snprintf(NULL, 0,
                             "{\"daemon\":\"channel_d\",\"channels\":%zu,"
                             "\"messages_sent\":%llu,\"messages_received\":%llu}",
                             count, (unsigned long long)sent, (unsigned long long)recv) +
                    1;
        char *buf = (char *)AIRY_MALLOC(sz);
        if (!buf) {
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for stats response buffer");
        }
        snprintf(buf, sz,
                 "{\"daemon\":\"channel_d\",\"channels\":%zu,"
                 "\"messages_sent\":%llu,\"messages_received\":%llu}",
                 count, (unsigned long long)sent, (unsigned long long)recv);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "open") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        cJSON *name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(name) ||
            !name->valuestring[0]) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or name\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id or name in open request");
        }

        channel_type_t type = CHANNEL_TYPE_SOCKET;
        cJSON *typej = cJSON_GetObjectItem(params, "type");
        if (cJSON_IsNumber(typej) && typej->valueint >= 0 && typej->valueint <= 2)
            type = (channel_type_t)typej->valueint;

        int rc = channel_service_open(svc, id->valuestring, name->valuestring, type, NULL);
        if (rc != 0) {
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"open failed: %d\"}", rc);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_open failed");
        }

        *response_json = AIRY_STRDUP("{\"status\":\"opened\"}");
        return 0;
    }

    if (strcmp(method, "close") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id in close request");
        }

        int rc = channel_service_close(svc, id->valuestring);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"close failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_close failed");
        }
        *response_json = AIRY_STRDUP("{\"status\":\"closed\"}");
        return 0;
    }

    if (strcmp(method, "send") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        cJSON *data = cJSON_GetObjectItem(params, "data");
        /* fail-closed: data must be a string; non-string (number/object/array)
         * or missing returns an error — silent skipping that loses messages
         * while reporting success is forbidden */
        if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(data)) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or data\"}");
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "missing id or data in send request");
        }
        size_t dlen = strlen(data->valuestring);
        int rc = channel_service_send(svc, id->valuestring, data->valuestring, dlen);
        if (rc != 0) {
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"send failed: %d\"}", rc);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_send failed");
        }
        *response_json = AIRY_STRDUP("{\"status\":\"sent\"}");
        return 0;
    }

    if (strcmp(method, "health") == 0 || strcmp(method, "health_check") == 0) {
        bool healthy = channel_service_is_healthy(svc);
        *response_json = AIRY_STRDUP(healthy ? "{\"healthy\":true}" : "{\"healthy\":false}");
        return 0;
    }

    *response_json = AIRY_STRDUP("{\"error\":\"unknown method\"}");
    AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown method");
}

/**
 * @brief Hand cJSON params directly to chan_rpc_call (cJSON parsing,
 *        no string round-trip) and wrap the returned JSON result as a
 *        JSON-RPC success response.
 *
 * user_data comes from daemon_handle_client_*, pointing to client_fd.
 */
static void channel_dispatch_method(cJSON *params, int id, void *user_data, const char *method)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;

    char *result_json = NULL;
    int rc = chan_rpc_call(method, params, &result_json, g_svc);
    if (rc != 0 || !result_json) {
        /* Param-validation failures (fail-closed: missing id/data, invalid
         * type) map to JSON-RPC Invalid params(-32602); other service errors
         * map to Internal error(-32603) */
        int code = (rc == AIRY_ERR_INVALID_PARAM) ? JSONRPC_INVALID_PARAMS : JSONRPC_INTERNAL_ERROR;
        JSONRPC_SEND_ERROR(client_fd, code, "channel service error", id);
        AIRY_FREE(result_json);
        return;
    }

    cJSON *result = cJSON_Parse(result_json);
    AIRY_FREE(result_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "invalid channel response", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

void m_ping(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "ping");
}

void m_list(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "list");
}

void m_open(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "open");
}

void m_close(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "close");
}

void m_send(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "send");
}

void m_health(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "health");
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "health");
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "stats");
}
