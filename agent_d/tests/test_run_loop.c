// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_loop.c
 * @brief 工具循环与编排失败语义直测（white-box include，无 daemon 依赖）：
 *        C-1 连败计数语义 + 流式增量上屏契约 + S-2 编排失败上抛。
 */

#include "../src/run/agent_run_loop.c"

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

/* C-14：终态名是 run_end / 决策链 / result 三处的唯一映射点。任务级未收敛
 * （空输出、熔断、预算耗尽）一律落 paused，产物已移交、客户端可继续或放弃；
 * 只有用户取消才落 failed，rc=0 落 completed，禁止静默停留 Executing。 */
static void test_run_state(void)
{
    CHECK(strcmp(run_state_name(0, 0), "completed") == 0, "state: rc=0 completed");
    CHECK(strcmp(run_state_name(0, 1), "failed") == 0, "state: cancel is failed");
    CHECK(strcmp(run_state_name(AGENT_RUN_RC_EMPTY, 0), "paused") == 0, "state: empty paused");
    CHECK(strcmp(run_state_name(AGENT_RUN_RC_TOOL_FUSE, 0), "paused") == 0, "state: fuse paused");
    CHECK(strcmp(run_state_name(AIRY_EBUDGET, 0), "paused") == 0, "state: ebudget paused");

    CHECK(run_rc_msg(0)[0] == '\0', "msg: success silent");
    CHECK(strstr(run_rc_msg(AGENT_RUN_RC_EMPTY), "without output") != NULL, "msg: empty named");
    CHECK(strstr(run_rc_msg(AGENT_RUN_RC_TOOL_FUSE), "fuse") != NULL, "msg: fuse named");
    CHECK(strstr(run_rc_msg(AIRY_EBUDGET), "resume_token") != NULL, "msg: ebudget resumable");
    CHECK(run_rc_msg(AGENT_RUN_RC_EMPTY) != run_rc_msg(AGENT_RUN_RC_TOOL_FUSE),
          "msg: terminal rcs distinct");

    CHECK(AGENT_RUN_RC_EMPTY == 6, "rc: empty code is 6");
}

/* C-14：同工具同参数连败达到阈值即强制换路——闸门必须在第二次相同失败后
 * 关闭，换参数/换工具则重新开口；任一成功清零，避免误伤后续重试。 */
static void test_switch_gate(void)
{
    const uint64_t sig = run_call_sig("shell_run", "{\"cmd\":\"curl a.example\"}");
    CHECK(sig == run_call_sig("shell_run", "{\"cmd\":\"curl a.example\"}"), "sig: stable");
    CHECK(sig != run_call_sig("shell_run", "{\"cmd\":\"curl b.example\"}"), "sig: args differ");
    CHECK(sig != run_call_sig("fs_read", "{\"cmd\":\"curl a.example\"}"), "sig: tool differ");

    uint64_t last = 0;
    int repeat = 0;
    CHECK(run_repeat_bump(&last, &repeat, sig, -1) == 1, "repeat: first failure");
    CHECK(!run_repeat_blocked(last, repeat, sig), "gate: open after 1 failure");
    CHECK(run_repeat_bump(&last, &repeat, sig, -1) == 2, "repeat: identical failure twice");
    CHECK(run_repeat_blocked(last, repeat, sig), "gate: closed after 2 failures");

    const uint64_t other = run_call_sig("fs_read", "{}");
    CHECK(!run_repeat_blocked(last, repeat, other), "gate: other call still open");
    CHECK(run_repeat_bump(&last, &repeat, other, -1) == 1, "repeat: other call resets count");
    CHECK(!run_repeat_blocked(last, repeat, sig), "gate: old signature no longer armed");

    CHECK(run_repeat_bump(&last, &repeat, other, 0) == 0, "repeat: success clears");
    CHECK(!run_repeat_blocked(last, repeat, other), "gate: success reopens");
}

/* C-14：续跑令牌必须 URL 安全、无填充，且无论诉求与步骤多长都收敛到硬上限；
 * 上限内放不下时只裁剪步骤摘要，不得丢弃令牌本身。 */
static void test_resume_token(void)
{
    char *a = run_b64url((const unsigned char *)"f", 1);
    char *b = run_b64url((const unsigned char *)"fo", 2);
    char *c = run_b64url((const unsigned char *)"foo", 3);
    char *d = run_b64url((const unsigned char *)"foob", 4);
    CHECK(a && strcmp(a, "Zg") == 0, "b64: one byte, no padding");
    CHECK(b && strcmp(b, "Zm8") == 0, "b64: two bytes, no padding");
    CHECK(c && strcmp(c, "Zm9v") == 0, "b64: three bytes exact");
    CHECK(d && strcmp(d, "Zm9vYg") == 0, "b64: four bytes url safe");
    AIRY_FREE(a);
    AIRY_FREE(b);
    AIRY_FREE(c);
    AIRY_FREE(d);

    char *prompt = (char *)AIRY_CALLOC(1, 4096);
    CHECK(prompt != NULL, "resume: prompt alloc");
    if (!prompt)
        return;
    AIRY_MEMSET(prompt, 'x', 4095);

    agent_run_loop_args_t args = {.prompt = prompt};
    agent_run_loop_result_t out = {0};
    run_ctx_t r = {.args = &args, .out = &out, .sess = "sess_test"};
    r.trace = cJSON_CreateArray();
    for (int i = 0; i < 20; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "tool", "shell_run");
        cJSON_AddStringToObject(it, "result", "permission denied on /var/log/app.log");
        cJSON_AddNumberToObject(it, "ok", i % 2);
        cJSON_AddItemToArray(r.trace, it);
    }
    r.text = AIRY_STRDUP("halfway there");
    out.rounds_limit = AGENT_RUN_ROUNDS_MAX;
    out.rounds_used = AGENT_RUN_ROUNDS_MAX;

    char *tok = run_resume_emit(&r);
    CHECK(tok != NULL, "resume: token emitted");
    CHECK(tok && strlen(tok) <= AGENT_RUN_RESUME_TOKEN_MAX, "resume: token within cap");
    int safe = tok != NULL;
    for (const char *p = tok; p && *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            safe = 0;
            break;
        }
    }
    CHECK(safe, "resume: url-safe alphabet only");
    AIRY_FREE(tok);
    cJSON_Delete(r.trace);
    AIRY_FREE(r.text);
    AIRY_FREE(prompt);
}

/* C-23：链事件定长字段（160 字节）截断必须停在完整 UTF-8 字符上。 */
static void test_fit_boundary(void)
{
    char buf[160];
    char src[200];

    memset(src, 'a', 158);
    src[158] = (char)0xE4;
    src[159] = (char)0xB8;
    src[160] = (char)0xAD;
    src[161] = '\0';
    run_fit(buf, sizeof(buf), src);
    CHECK(strlen(buf) == 158, "fit: split multi-byte char dropped");
    CHECK(utf8_validate(buf, strlen(buf)), "fit: result is valid UTF-8");

    run_fit(buf, sizeof(buf), "紧凑");
    CHECK(strcmp(buf, "紧凑") == 0, "fit: short UTF-8 verbatim");

    run_fit(buf, sizeof(buf), NULL);
    CHECK(buf[0] == '\0', "fit: NULL becomes empty");
}

/* C-23：链事件落盘前必须同时满足 UTF-8 合法与 JSON 可解析。 */
static void test_evt_line(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "k", "值");
    char *line = cJSON_PrintUnformatted(obj);
    CHECK(line && agent_evt_ok(line) == 1, "evt: valid UTF-8 JSON accepted");
    AIRY_FREE(line);
    cJSON_Delete(obj);

    CHECK(agent_evt_ok("{\"k\":1}") == 1, "evt: compact ASCII JSON accepted");
    CHECK(agent_evt_ok("{\"k\":") == 0, "evt: truncated JSON rejected");
    const char bad[] = {'{', '"', 'k', '"', ':', '"', (char)0xE4, (char)0xB8, '"', '}', '\0'};
    CHECK(agent_evt_ok(bad) == 0, "evt: invalid UTF-8 rejected");
    CHECK(agent_evt_ok(NULL) == 0, "evt: NULL rejected");
}

int main(void)
{
    test_fail_streak();
    test_emit_delta();
    test_stream_result_free();
    test_orch_contract();
    test_run_state();
    test_switch_gate();
    test_resume_token();
    test_fit_boundary();
    test_evt_line();
    if (g_fail)
        printf("FAILURES: %d\n", g_fail);
    else
        printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
