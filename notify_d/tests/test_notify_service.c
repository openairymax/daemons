// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_notify_service.c
 * @brief 通知服务核心单元测试
 *
 * 覆盖：
 *  - publish / enqueue 后事件进入环形队列
 *  - subscribe / unsubscribe 对 topic 订阅注册表的增删
 *  - JSON-RPC 方法分发响应为合法 JSON（publish/subscribe/unsubscribe/list/health/
 *    get_stats/health_check/shutdown）
 *  - 订阅注册表驱动的 topic 过滤广播（POSIX socketpair 端到端）
 *
 * 0.1.9 M4-S3：订阅键 wire 名为 topic（原 channel 语义改名，与
 * channel_d 数据面传输通道划清界限）。
 *
 */

#include "notify_service.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

static notify_d_service_t g_svc;

static void test_init_destroy(void)
{
    printf("  test_init_destroy...\n");
    assert(notify_d_service_init(&g_svc) == AIRY_SUCCESS);
    assert(g_svc.pending_count == 0);
    assert(g_svc.subscription_count == 0);
    assert(g_svc.client_count == 0);
    notify_d_service_destroy(&g_svc);
    printf("    PASSED\n");
}

static void test_publish_enqueues_event(void)
{
    printf("  test_publish_enqueues_event...\n");
    notify_d_service_init(&g_svc);

    assert(notify_d_enqueue(&g_svc, "hello notify", "alerts", "alert") == AIRY_SUCCESS);
    assert(g_svc.pending_count == 1);
    assert(g_svc.pending[g_svc.pending_head] != NULL);
    assert(strcmp(g_svc.pending[g_svc.pending_head]->message, "hello notify") == 0);
    assert(strcmp(g_svc.pending[g_svc.pending_head]->topic, "alerts") == 0);
    assert(strcmp(g_svc.pending[g_svc.pending_head]->event_type, "alert") == 0);

    char resp[4096];
    int rc = notify_d_dispatch_jsonrpc(
        &g_svc,
        "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{\"event\":\"deploy\","
        "\"topic\":\"ops\",\"message\":\"build #42 ok\"},\"id\":7}",
        resp, sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_HANDLED);
    assert(g_svc.pending_count == 2);
    size_t tail = (g_svc.pending_head + 1) % NOTIFY_D_MAX_PENDING;
    assert(strcmp(g_svc.pending[tail]->message, "build #42 ok") == 0);
    assert(strcmp(g_svc.pending[tail]->topic, "ops") == 0);
    assert(strcmp(g_svc.pending[tail]->event_type, "deploy") == 0);

    rc = notify_d_dispatch_jsonrpc(
        &g_svc,
        "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{\"payload\":\"via payload\"},"
        "\"id\":8}",
        resp, sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_HANDLED);
    assert(g_svc.pending_count == 3);
    size_t tail2 = (g_svc.pending_head + 2) % NOTIFY_D_MAX_PENDING;
    assert(strcmp(g_svc.pending[tail2]->message, "via payload") == 0);
    assert(strcmp(g_svc.pending[tail2]->topic, "default") == 0);
    assert(strcmp(g_svc.pending[tail2]->event_type, "message") == 0);

    g_svc.pending_count = NOTIFY_D_MAX_PENDING;
    rc = notify_d_dispatch_jsonrpc(&g_svc,
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{"
                                   "\"message\":\"overflow\"},\"id\":9}",
                                   resp, sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_HANDLED);
    cJSON *parsed = cJSON_Parse(resp);
    assert(parsed != NULL);
    assert(cJSON_IsObject(cJSON_GetObjectItem(parsed, "error")));
    cJSON_Delete(parsed);
    g_svc.pending_count = 3;

    notify_d_service_destroy(&g_svc);
    printf("    PASSED\n");
}

static void test_subscribe_unsubscribe(void)
{
    printf("  test_subscribe_unsubscribe...\n");
    notify_d_service_init(&g_svc);

    assert(notify_d_subscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);
    assert(notify_d_subscribe(&g_svc, "alerts", "agent-2") == AIRY_SUCCESS);
    assert(notify_d_subscribe(&g_svc, "ops", "agent-1") == AIRY_SUCCESS);
    assert(notify_d_subscription_count(&g_svc, "alerts") == 2);
    assert(notify_d_subscription_count(&g_svc, "ops") == 1);
    assert(notify_d_subscription_count(&g_svc, "none") == 0);
    assert(notify_d_has_subscription(&g_svc, "alerts", "agent-1"));
    assert(notify_d_has_subscription(&g_svc, "alerts", "agent-2"));
    assert(!notify_d_has_subscription(&g_svc, "alerts", "ghost"));

    assert(notify_d_subscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);
    assert(notify_d_subscription_count(&g_svc, "alerts") == 2);

    assert(notify_d_unsubscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);
    assert(notify_d_subscription_count(&g_svc, "alerts") == 1);
    assert(!notify_d_has_subscription(&g_svc, "alerts", "agent-1"));
    assert(notify_d_has_subscription(&g_svc, "ops", "agent-1"));

    assert(notify_d_unsubscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);

    assert(notify_d_subscribe(&g_svc, NULL, "x") != AIRY_SUCCESS);
    assert(notify_d_subscribe(&g_svc, "", "x") != AIRY_SUCCESS);
    assert(notify_d_subscribe(&g_svc, "x", NULL) != AIRY_SUCCESS);
    assert(notify_d_unsubscribe(&g_svc, NULL, "x") != AIRY_SUCCESS);

    assert(notify_d_subscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);
    assert(g_svc.subscription_count == 3);

    notify_d_service_destroy(&g_svc);
    printf("    PASSED\n");
}

static void test_dispatch_responses_valid_json(void)
{
    printf("  test_dispatch_responses_valid_json...\n");
    notify_d_service_init(&g_svc);

    const char *reqs[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"health\",\"params\":{},\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"get_stats\",\"params\":{},\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"health_check\",\"params\":{},\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"list\",\"params\":{},\"id\":4}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"subscribe\",\"params\":{\"topic\":\"alerts\","
        "\"client_id\":\"agent-9\"},\"id\":5}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"unsubscribe\",\"params\":{\"topic\":\"alerts\","
        "\"client_id\":\"agent-9\"},\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{\"topic\":\"alerts\","
        "\"message\":\"ping\"},\"id\":9}",
    };
    for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
        char resp[8192];
        int rc = notify_d_dispatch_jsonrpc(&g_svc, reqs[i], resp, sizeof(resp));
        assert(rc == NOTIFY_D_METHOD_HANDLED);

        cJSON *parsed = cJSON_Parse(resp);
        assert(parsed != NULL);
        cJSON *jsonrpc = cJSON_GetObjectItem(parsed, "jsonrpc");
        assert(cJSON_IsString(jsonrpc));
        assert(strcmp(jsonrpc->valuestring, "2.0") == 0);
        cJSON *result = cJSON_GetObjectItem(parsed, "result");
        assert(cJSON_IsObject(result));
        cJSON *id = cJSON_GetObjectItem(parsed, "id");
        assert(cJSON_IsNumber(id));
        cJSON_Delete(parsed);
    }

    char resp[8192];
    notify_d_dispatch_jsonrpc(
        &g_svc,
        "{\"jsonrpc\":\"2.0\",\"method\":\"subscribe\",\"params\":{\"topic\":\"alerts\","
        "\"client_id\":\"agent-9\"},\"id\":5}",
        resp, sizeof(resp));
    notify_d_dispatch_jsonrpc(&g_svc,
                              "{\"jsonrpc\":\"2.0\",\"method\":\"list\",\"params\":{},\"id\":4}",
                              resp, sizeof(resp));
    cJSON *parsed = cJSON_Parse(resp);
    assert(parsed != NULL);
    cJSON *result = cJSON_GetObjectItem(parsed, "result");
    cJSON *topics = cJSON_GetObjectItem(result, "topics");
    assert(cJSON_IsArray(topics));
    int found = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, topics)
    {
        cJSON *tp = cJSON_GetObjectItem(item, "topic");
        assert(cJSON_IsString(tp));
        if (strcmp(tp->valuestring, "alerts") == 0) {
            found = 1;
            cJSON *subs = cJSON_GetObjectItem(item, "subscribers");
            assert(cJSON_IsNumber(subs));
            assert(subs->valueint == 1);
        }
    }
    assert(found);
    cJSON_Delete(parsed);

    notify_d_dispatch_jsonrpc(&g_svc,
                              "{\"jsonrpc\":\"2.0\",\"method\":\"health\",\"params\":{},\"id\":1}",
                              resp, sizeof(resp));
    parsed = cJSON_Parse(resp);
    assert(parsed != NULL);
    result = cJSON_GetObjectItem(parsed, "result");
    assert(cJSON_IsObject(result));
    cJSON *queue_pending = cJSON_GetObjectItem(result, "queue_pending");
    cJSON *queue_capacity = cJSON_GetObjectItem(result, "queue_capacity");
    assert(cJSON_IsNumber(queue_pending));
    assert(cJSON_IsNumber(queue_capacity));
    assert(queue_capacity->valueint == NOTIFY_D_MAX_PENDING);
    cJSON_Delete(parsed);

    int rc = notify_d_dispatch_jsonrpc(
        &g_svc, "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{},\"id\":10}", resp,
        sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_HANDLED);
    parsed = cJSON_Parse(resp);
    assert(parsed != NULL);
    assert(cJSON_IsObject(cJSON_GetObjectItem(parsed, "error")));
    cJSON_Delete(parsed);

    rc = notify_d_dispatch_jsonrpc(
        &g_svc, "{\"jsonrpc\":\"2.0\",\"method\":\"shutdown\",\"params\":{},\"id\":11}", resp,
        sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_SHUTDOWN);
    parsed = cJSON_Parse(resp);
    assert(parsed != NULL);
    result = cJSON_GetObjectItem(parsed, "result");
    assert(cJSON_IsObject(result));
    cJSON_Delete(parsed);

    rc = notify_d_dispatch_jsonrpc(
        &g_svc, "{\"jsonrpc\":\"2.0\",\"method\":\"bogus\",\"params\":{},\"id\":12}", resp,
        sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_NOT_RPC);

    rc = notify_d_dispatch_jsonrpc(&g_svc, "hello raw message", resp, sizeof(resp));
    assert(rc == NOTIFY_D_METHOD_NOT_RPC);

    notify_d_service_destroy(&g_svc);
    printf("    PASSED\n");
}

#ifndef _WIN32

static void test_broadcast_to_subscribed_client(void)
{
    printf("  test_broadcast_to_subscribed_client...\n");
    notify_d_service_init(&g_svc);

    int sp[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);

    assert(notify_d_subscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);

    notify_client_t *client = &g_svc.clients[0];
    __builtin_memset(client, 0, sizeof(*client));
    client->fd = sp[0];
    client->type = NOTIFY_CLIENT_SOCKET;
    client->active = 1;
    client->client_id = AIRY_STRDUP("agent-1");
    client->topic = AIRY_STRDUP("none");
    g_svc.client_count = 1;

    notify_event_t ev = {0};
    ev.message = "alert msg";
    ev.topic = "alerts";
    ev.event_type = "alert";
    ev.timestamp = 12345;
    assert(notify_d_broadcast_event(&g_svc, &ev) == 1);

    char buf[4096];
    ssize_t got = recv(sp[1], buf, sizeof(buf) - 1, 0);
    assert(got > 0);
    buf[got] = '\0';
    assert(strstr(buf, "\"alert msg\"") != NULL);
    assert(strstr(buf, "\"alerts\"") != NULL);

    ev.topic = "other";
    assert(notify_d_broadcast_event(&g_svc, &ev) == 0);
    got = recv(sp[1], buf, sizeof(buf), MSG_DONTWAIT);
    assert(got < 0); /* EAGAIN */

    assert(notify_d_unsubscribe(&g_svc, "alerts", "agent-1") == AIRY_SUCCESS);
    ev.topic = "alerts";
    assert(notify_d_broadcast_event(&g_svc, &ev) == 0);
    got = recv(sp[1], buf, sizeof(buf), MSG_DONTWAIT);
    assert(got < 0);

    close(sp[0]);
    close(sp[1]);
    notify_d_service_destroy(&g_svc);
    printf("    PASSED\n");
}
#endif

int main(void)
{
    printf("=== Notify Service Unit Tests ===\n");
    test_init_destroy();
    test_publish_enqueues_event();
    test_subscribe_unsubscribe();
    test_dispatch_responses_valid_json();
#ifndef _WIN32
    test_broadcast_to_subscribed_client();
#endif
    printf("=== All tests PASSED ===\n");
    return 0;
}
