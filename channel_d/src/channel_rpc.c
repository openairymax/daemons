// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_rpc.c
 * @brief channel.* RPC 方法域：ping / list / open / close / send /
 *        health / health_check / get_stats。
 *
 * 0.1.19 W1 显性塌缩（类 III·daemons RPC 族首件）：删除 chan_rpc_call
 * 漏斗。method_dispatcher 已按方法名派发，漏斗内的 strcmp 链是第二次
 * 同义派发；结果改由 cJSON 直接构造，消去「snprintf 拼串 → cJSON_Parse
 * 回环」——原写法对 channel_id / name 不做 JSON 转义，含引号或反斜杠的
 * 通道名会产出非法 JSON 并让请求以 "invalid channel response" 失败。
 * 通道快照表（channel_info_t × CHANNEL_MAX_CHANNELS ≈ 240 KB）由栈改
 * 堆承载，端侧受限线程栈下不再构成溢出风险。
 *
 * m_* 壳族收敛到 DAEMON_RPC_SHELL 机制件（daemon_main.h，SSoT）。wire
 * 键名与 gateway_cap_registry 的 channel.* FWD 契约保持一致。
 *
 * 响应码订正：原 `rc == AIRY_ERR_INVALID_PARAM` 比较恒不成立（AIRY_ERROR
 * 经 AIRY_ERR_NEG 返回负值），缺 id/data 与非法类型一律退化为 -32603；
 * 本域经 JSONRPC_REQUIRE 直发 Invalid params(-32602)，与本文件既有文档
 * 契约及 L2 服务协议一致。服务错误仍映射 Internal error(-32603)。
 */

#include "airy_memory.h"
#include "channel_d_internal.h"
#include "svc_channel_d.h"

#include "daemon_main.h"
#include "param_validator.h"

#include <stdio.h>
#include <string.h>

/* channel_id 取值：字符串直取，数字 id 规范化为十进制串（手写
 * strstr/strchr 解析不支持数字 id 与转义引号）。返回 AIRY_MALLOC
 * 串，无有效 id 时返回 NULL。 */
static char *chan_id_str(cJSON *params)
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

/* 通道快照：全表容量不入调用栈，由堆承载。成功返回表基址并置
 * *out_count，失败（含服务错误）返回 NULL 且 *out_count 归零。 */
static channel_info_t *chan_snapshot(size_t *out_count)
{
    channel_info_t *infos =
        (channel_info_t *)AIRY_MALLOC(sizeof(*infos) * CHANNEL_MAX_CHANNELS);

    *out_count = 0;
    if (!infos)
        return NULL;
    if (channel_service_list(g_svc, infos, CHANNEL_MAX_CHANNELS, out_count) != CHANNEL_OK) {
        AIRY_FREE(infos);
        *out_count = 0;
        return NULL;
    }
    return infos;
}

static void handle_ping(cJSON *params, int id, airy_sock_t fd)
{
    char *chan = chan_id_str(params);
    if (!chan) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status",
                                channel_service_is_healthy(g_svc) ? "ok" : "degraded");
        JSONRPC_SEND_SUCCESS(fd, result, id);
        return;
    }

    int64_t latency_ms = 0;
    if (channel_service_ping(g_svc, chan, &latency_ms) != CHANNEL_OK) {
        AIRY_FREE(chan);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "ping failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddStringToObject(result, "channel_id", chan);
    cJSON_AddNumberToObject(result, "latency_ms", (double)latency_ms);
    AIRY_FREE(chan);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_list(int id, airy_sock_t fd)
{
    size_t count = 0;
    channel_info_t *infos = chan_snapshot(&count);
    if (!infos) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "list failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddStringToObject(ch, "id", infos[i].channel_id);
        cJSON_AddStringToObject(ch, "name", infos[i].name);
        cJSON_AddNumberToObject(ch, "type", (double)infos[i].type);
        cJSON_AddNumberToObject(ch, "status", (double)infos[i].status);
        cJSON_AddNumberToObject(ch, "sent", (double)infos[i].messages_sent);
        cJSON_AddNumberToObject(ch, "recv", (double)infos[i].messages_received);
        cJSON_AddItemToArray(arr, ch);
    }
    AIRY_FREE(infos);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "channels", arr);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_stats(int id, airy_sock_t fd)
{
    size_t count = 0;
    channel_info_t *infos = chan_snapshot(&count);
    if (!infos) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "stats failed", id);
        return;
    }

    uint64_t sent = 0;
    uint64_t recv = 0;
    for (size_t i = 0; i < count; i++) {
        sent += infos[i].messages_sent;
        recv += infos[i].messages_received;
    }
    AIRY_FREE(infos);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "channel_d");
    cJSON_AddNumberToObject(result, "channels", (double)count);
    cJSON_AddNumberToObject(result, "messages_sent", (double)sent);
    cJSON_AddNumberToObject(result, "messages_received", (double)recv);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_open(cJSON *params, int id, airy_sock_t fd)
{
    const char *chan = get_string_field(params, "id", NULL);
    const char *name = get_string_field(params, "name", NULL);
    JSONRPC_REQUIRE(chan && chan[0] && name && name[0], fd, "Missing id or name", id);

    double raw = get_double_field(params, "type", (double)CHANNEL_TYPE_SOCKET);
    channel_type_t type = (raw >= (double)CHANNEL_TYPE_SOCKET &&
                           raw <= (double)CHANNEL_TYPE_PIPE)
                              ? (channel_type_t)raw
                              : CHANNEL_TYPE_SOCKET;

    if (channel_service_open(g_svc, chan, name, type, NULL) != CHANNEL_OK) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "open failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "opened");
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_close(cJSON *params, int id, airy_sock_t fd)
{
    const char *chan = get_string_field(params, "id", NULL);
    JSONRPC_REQUIRE(chan && chan[0], fd, "Missing id", id);

    if (channel_service_close(g_svc, chan) != CHANNEL_OK) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "close failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "closed");
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_send(cJSON *params, int id, airy_sock_t fd)
{
    const char *chan = get_string_field(params, "id", NULL);
    const char *data = get_string_field(params, "data", NULL);
    /* fail-closed：data 缺失或非字符串（数字/对象/数组）一律拒绝，
     * 禁止静默丢弃消息却回报成功。 */
    JSONRPC_REQUIRE(chan && chan[0] && data, fd, "Missing id or data", id);

    if (channel_service_send(g_svc, chan, data, strlen(data)) != CHANNEL_OK) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "send failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "sent");
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_health(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "healthy", channel_service_is_healthy(g_svc));
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

DAEMON_RPC_SHELL(ping, handle_ping)
DAEMON_RPC_SHELL(open, handle_open)
DAEMON_RPC_SHELL(close, handle_close)
DAEMON_RPC_SHELL(send, handle_send)
DAEMON_RPC_SHELL0(list, handle_list)
DAEMON_RPC_SHELL0(get_stats, handle_stats)
DAEMON_RPC_SHELL0(health, handle_health)
/* 协议别名：health_check 与 health 同实现（.manifest rpc.methods 并列）。 */
DAEMON_RPC_SHELL0(health_check, handle_health)
