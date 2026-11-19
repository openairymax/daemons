// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_open.c
 * @brief 无模型配置时的失败载荷契约直测（white-box include，无 daemon 依赖）。
 *
 * 锁死「空配置首轮失败」对客户端的可操作性：origin=config、
 * reason=config_missing、retryable=false、next_action=inspect。任何一项
 * 漂移都会让 TUI/CLI 把「去配置模型」误显示成「重试」或未知错误。
 */

#include "../src/run/agent_run_exec.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 引擎的 daemon 生命周期挂钩：产品侧由 main.c 定义，测试进程无服务实例
 * （NULL 即未就绪语义，与 main.c 初始值一致）。 */
#include "agent_d_internal.h"
agent_service_t *g_service = NULL;

static int g_fail;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

static cJSON *g_evt;
static char g_type[32];

/* sink 只读：信封所有权留在引擎侧（emit 返回后即释放），此处深拷贝留证。 */
static void cap_emit(const char *type, cJSON *env, void *ud)
{
    (void)ud;
    snprintf(g_type, sizeof(g_type), "%s", type ? type : "");
    if (g_evt)
        cJSON_Delete(g_evt);
    g_evt = env ? cJSON_Duplicate(env, 1) : NULL;
}

static void fail_field(const cJSON *fail, const char *key, const char *want, const char *msg)
{
    const cJSON *v = fail ? cJSON_GetObjectItem(fail, key) : NULL;
    CHECK(cJSON_IsString(v) && strcmp(v->valuestring, want) == 0, msg);
}

static void test_config_missing_payload(void)
{
    char tmpl[] = "/tmp/airy-run-open-XXXXXX";
    char *home = mkdtemp(tmpl);
    CHECK(home != NULL, "open: temp home created");
    if (!home)
        return;
    setenv("AIRY_HOME", home, 1);
    unsetenv("AIRY_AGENT_MODEL");

    agent_run_event_sink_t sink = {cap_emit, NULL};
    run_ctx_t c;
    AIRY_MEMSET(&c, 0, sizeof(c));
    c.prompt = "hi";
    c.sink = &sink;
    g_type[0] = '\0';

    CHECK(run_open(&c) == -1, "open: fails closed without model");
    CHECK(strcmp(g_type, AIRY_RS_TYPE_ERROR) == 0, "open: emits error event");
    CHECK(c.active == NULL, "open: session unregistered");

    const cJSON *data = g_evt ? cJSON_GetObjectItem(g_evt, AIRY_RS_K_DATA) : NULL;
    CHECK(cJSON_IsObject(data), "open: error payload present");
    const cJSON *fail = data ? cJSON_GetObjectItem(data, AIRY_RS_K_FAILURE) : NULL;
    CHECK(cJSON_IsObject(fail), "open: failure object present");
    if (fail) {
        fail_field(fail, AIRY_RS_K_ORIGIN, AIRY_RS_ORIGIN_CONFIG, "fail: origin=config");
        fail_field(fail, AIRY_RS_K_REASON, AIRY_RS_REASON_CONFIG, "fail: reason=config_missing");
        fail_field(fail, AIRY_RS_K_NEXT_ACTION, AIRY_RS_NEXT_INSPECT, "fail: next_action=inspect");
        const cJSON *retry = cJSON_GetObjectItem(fail, AIRY_RS_K_RETRYABLE);
        CHECK(cJSON_IsFalse(retry), "fail: not retryable");
    }

    if (g_evt) {
        cJSON_Delete(g_evt);
        g_evt = NULL;
    }
    (void)rmdir(home);
}

int main(void)
{
    test_config_missing_payload();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_run_open: all checks passed\n");
    return 0;
}
