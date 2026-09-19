// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_loop.c
 * @brief 工具循环与编排失败语义直测（white-box include，无 daemon 依赖）：
 *        C-1 连败计数语义 + 流式增量上屏契约 + S-2 编排失败上抛。
 */

#include "../src/agent_run_loop.c"

#include <stdio.h>

/* engine.c 引用的 daemon 全局服务句柄：产品侧由 main.c 定义，测试环境
 * 无 daemon 生命周期，NULL 即未就绪语义（与 main.c 初始值一致）。 */
#include "agent_d_internal.h"
agent_service_t *g_service = NULL;

static int g_fail = 0;

#define CHECK(cond, name)                \
    do {                                 \
        if (cond) {                      \
            printf("PASS %s\n", (name)); \
        } else {                         \
            printf("FAIL %s\n", (name)); \
            g_fail++;                    \
        }                                \
    } while (0)

static void test_fail_streak(void)
{
    int s = 0;
    CHECK(!run_fail_streak_bump(&s, 0) && s == 0, "bump: success resets to zero");
    CHECK(!run_fail_streak_bump(&s, -1) && s == 1, "bump: first failure counts");
    CHECK(!run_fail_streak_bump(&s, -1) && s == 2, "bump: streak accumulates");
    CHECK(run_fail_streak_bump(&s, -1) && s == 3, "bump: fuse trips at K");
    CHECK(run_fail_streak_bump(&s, -1) && s == 4, "bump: stays tripped");
    CHECK(!run_fail_streak_bump(&s, 0) && s == 0, "bump: success clears tripped state");
    CHECK(AGENT_RUN_TOOL_FAIL_LIMIT == 3, "bump: K constant is 3");
}

/* run_emit_delta：真实增量即时上屏（B2 流式契约）。
 * 校验载荷以 dlen 为准（非 strlen）、空增量不产生事件、大载荷不截断。 */
typedef struct {
    int calls;
    char type[48];
    char delta[2048];
    size_t dlen;
} delta_cap_t;

static void cap_emit(const char *type, cJSON *env, void *ud)
{
    delta_cap_t *c = (delta_cap_t *)ud;
    c->calls++;
    AIRY_STRNCPY_TERM(c->type, type ? type : "", sizeof(c->type));
    cJSON *data = env ? cJSON_GetObjectItem(env, AIRY_RS_K_DATA) : NULL;
    cJSON *dV = data ? cJSON_GetObjectItem(data, AIRY_RS_K_DELTA) : NULL;
    if (cJSON_IsString(dV)) {
        c->dlen = strlen(dV->valuestring);
        AIRY_STRNCPY_TERM(c->delta, dV->valuestring, sizeof(c->delta));
    }
}

static void test_emit_delta(void)
{
    delta_cap_t cap;
    memset(&cap, 0, sizeof(cap));
    agent_run_event_sink_t sink = {cap_emit, &cap};
    uint64_t seq = 0;
    run_delta_ctx_t ctx = {&sink, &seq, "run-1", "sess-1"};

    run_emit_delta("hello", 5, &ctx);
    CHECK(cap.calls == 1 && strcmp(cap.type, AIRY_RS_TYPE_TOKEN_DELTA) == 0,
          "delta: one token_delta event");
    CHECK(strcmp(cap.delta, "hello") == 0 && cap.dlen == 5, "delta: payload verbatim");

    /* dlen 权威：缓冲区更长也只推出前 dlen 字节。 */
    run_emit_delta("abcdef", 3, &ctx);
    CHECK(cap.calls == 2 && strcmp(cap.delta, "abc") == 0, "delta: length authoritative");

    run_emit_delta("ignored", 0, &ctx);
    run_emit_delta(NULL, 7, &ctx);
    CHECK(cap.calls == 2, "delta: empty/NULL payload emits nothing");

    run_emit_delta("x", 1, NULL);
    CHECK(cap.calls == 2, "delta: NULL ctx is a no-op");

    /* V2.2：超过旧 512B 上限的增量必须全量承载，不得截断。 */
    char big[901];
    for (size_t i = 0; i < 900; i++)
        big[i] = (char)('a' + (int)(i % 26));
    big[900] = '\0';
    run_emit_delta(big, 900, &ctx);
    CHECK(cap.calls == 3 && cap.dlen == 900 && memcmp(cap.delta, big, 900) == 0,
          "delta: 900B payload not truncated");
}

static void test_stream_result_free(void)
{
    agent_stream_result_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.text = AIRY_STRDUP("t");
    sr.reason = AIRY_STRDUP("r");
    sr.tools = cJSON_CreateArray();
    sr.tokens = 12;
    sr.cost = 0.5;
    sr.error_code = -32603;
    AIRY_STRNCPY_TERM(sr.error_msg, "boom", sizeof(sr.error_msg));

    run_stream_result_free(&sr);
    CHECK(!sr.text && !sr.reason && !sr.tools, "free: owned pointers released");
    CHECK(sr.tokens == 0 && sr.cost == 0.0 && sr.error_code == 0 && sr.error_msg[0] == '\0',
          "free: state reset");
    run_stream_result_free(&sr);
    CHECK(!sr.text && !sr.tools, "free: idempotent on cleared result");
}

/* S-2：编排分支失败必须给出可判读原因（阶段 + 契约错误符号 + 原码），
 * 且终局失败档 rc 与 C-1 熔断档区分。 */
static void test_orch_contract(void)
{
    char *text = NULL;
    char *err = NULL;

    CHECK(agent_run_orchestrate(NULL, "p", &text, &err) != 0 && !text && err,
          "orch: non-object spec rejected");
    CHECK(err && strstr(err, "JSON object") != NULL, "orch: rejection reason readable");
    AIRY_FREE(err);

    cJSON *spec = cJSON_CreateObject();
    cJSON_AddStringToObject(spec, "role", "worker");
    err = NULL;
    /* 测试进程无 daemon 生命周期（g_service == NULL），spawn 必失败。 */
    CHECK(agent_run_orchestrate(spec, "p", &text, &err) != 0 && !text, "orch: spawn failure rc");
    CHECK(err && strstr(err, "agent.spawn failed") != NULL, "orch: failure stage named");
    CHECK(err && strstr(err, "ERR_INVALID_PARAM") != NULL, "orch: err symbol present");
    CHECK(err && strstr(err, "(-36)") != NULL, "orch: raw code present");
    AIRY_FREE(err);
    cJSON_Delete(spec);

    CHECK(AGENT_RUN_RC_SUBAGENT_FAIL == 3, "orch: subagent fail rc is 3");
    CHECK(AGENT_RUN_RC_SUBAGENT_FAIL != AGENT_RUN_RC_TOOL_FUSE, "orch: terminal rcs distinct");
}

int main(void)
{
    test_fail_streak();
    test_emit_delta();
    test_stream_result_free();
    test_orch_contract();
    if (g_fail)
        printf("FAILURES: %d\n", g_fail);
    else
        printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
