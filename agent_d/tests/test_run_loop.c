// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_loop.c
 * @brief 工具循环与编排失败语义直测（white-box include，无 daemon 依赖）：
 *        C-1 连败计数语义 + 流式增量上屏契约 + S-2 编排失败上抛。
 */

#include "../src/run/agent_run_loop.c"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

/* run_tool_params：审计主体随调用下发（V-05）。带 agent_id 时原样入参，
 * 缺位（NULL/空串）时不下发该键——tool_d 留痕据此记审计主体，不冒名。 */
static void test_tool_params_agent(void)
{
    char *p = run_tool_params("fs_read", "{\"path\":\"/tmp\"}", true, "coding_v1", NULL);
    CHECK(p != NULL, "params: build with agent id");
    cJSON *params = p ? cJSON_Parse(p) : NULL;
    CHECK(params != NULL, "params: parseable JSON");
    cJSON *aid = params ? cJSON_GetObjectItem(params, "agent_id") : NULL;
    CHECK(cJSON_IsString(aid) && strcmp(aid->valuestring, "coding_v1") == 0,
          "params: agent id verbatim");
    cJSON *dg = params ? cJSON_GetObjectItem(params, "allow_dangerous") : NULL;
    CHECK(cJSON_IsBool(dg) && cJSON_IsTrue(dg), "params: allow_dangerous preserved");
    AIRY_FREE(p);
    cJSON_Delete(params);

    p = run_tool_params("fs_read", "{\"path\":\"/tmp\"}", false, NULL, NULL);
    params = p ? cJSON_Parse(p) : NULL;
    CHECK(params != NULL && cJSON_GetObjectItem(params, "agent_id") == NULL,
          "params: missing id sends no key");
    AIRY_FREE(p);
    cJSON_Delete(params);

    p = run_tool_params("fs_read", "{\"path\":\"/tmp\"}", false, "", "");
    params = p ? cJSON_Parse(p) : NULL;
    CHECK(params != NULL && cJSON_GetObjectItem(params, "agent_id") == NULL,
          "params: empty id sends no key");
    AIRY_FREE(p);
    cJSON_Delete(params);

    /* N-9：凭据缺位（NULL/空串）不下发 approval_request_id；非空原样下发，
     * tool_d 以它作交互审批 pending 键，裁决回帧据此对上本次执行。 */
    p = run_tool_params("fs_read", "{\"path\":\"/tmp\"}", false, NULL, NULL);
    params = p ? cJSON_Parse(p) : NULL;
    CHECK(params != NULL && cJSON_GetObjectItem(params, "approval_request_id") == NULL,
          "params: missing approval id sends no key");
    AIRY_FREE(p);
    cJSON_Delete(params);

    p = run_tool_params("fs_read", "{\"path\":\"/tmp\"}", false, NULL, "a7");
    params = p ? cJSON_Parse(p) : NULL;
    cJSON *rid = params ? cJSON_GetObjectItem(params, "approval_request_id") : NULL;
    CHECK(cJSON_IsString(rid) && strcmp(rid->valuestring, "a7") == 0,
          "params: approval id verbatim");
    AIRY_FREE(p);
    cJSON_Delete(params);
}

/* N-9：审批预检的 mock tool_d。逐连接回 canned 响应（单请求单响应即关，
 * 与守护的真实形态一致），stop 置位并收到唤醒连接后退出。 */
typedef struct {
    const char *path;
    const char *resp;
    volatile int stop;
} probe_srv_t;

static void *probe_srv_run(void *ud)
{
    probe_srv_t *s = (probe_srv_t *)ud;
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
        return NULL;
    struct sockaddr_un a;
    AIRY_MEMSET(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", s->path);
    unlink(s->path);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 4) != 0) {
        close(lfd);
        return NULL;
    }
    while (!s->stop) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0)
            break;
        char b[1024];
        /* 单请求单响应：读到请求即回包。读等待 EOF 会让客户端等响应的同时
         * 服务端等关闭，5s 预算后客户端先关连接，服务端再写即 SIGPIPE；
         * 唤醒连接不带请求（读到 0），同样不得回包。 */
        ssize_t got = read(c, b, sizeof(b));
        if (s->resp && got > 0) {
            ssize_t w = write(c, s->resp, strlen(s->resp));
            (void)w;
        }
        close(c);
    }
    close(lfd);
    unlink(s->path);
    return NULL;
}

/* control_request 帧捕获：只取 data 段的平铺四键（TUI ctl.rs 契约）。 */
typedef struct {
    int calls;
    char type[48];
    char rid[64];
    char tool[64];
    char input[256];
    long long tmo;
} appr_cap_t;

static void appr_emit(const char *type, cJSON *env, void *ud)
{
    appr_cap_t *c = (appr_cap_t *)ud;
    c->calls++;
    AIRY_STRNCPY_TERM(c->type, type ? type : "", sizeof(c->type));
    cJSON *data = env ? cJSON_GetObjectItem(env, AIRY_RS_K_DATA) : NULL;
    cJSON *rid = data ? cJSON_GetObjectItem(data, AIRY_RS_K_REQUEST_ID) : NULL;
    cJSON *tool = data ? cJSON_GetObjectItem(data, AIRY_RS_K_TOOL) : NULL;
    cJSON *input = data ? cJSON_GetObjectItem(data, AIRY_RS_K_INPUT) : NULL;
    cJSON *tmo = data ? cJSON_GetObjectItem(data, AIRY_RS_K_TIMEOUT_MS) : NULL;
    if (cJSON_IsString(rid))
        AIRY_STRNCPY_TERM(c->rid, rid->valuestring, sizeof(c->rid));
    if (cJSON_IsString(tool))
        AIRY_STRNCPY_TERM(c->tool, tool->valuestring, sizeof(c->tool));
    if (cJSON_IsString(input))
        AIRY_STRNCPY_TERM(c->input, input->valuestring, sizeof(c->input));
    if (cJSON_IsNumber(tmo))
        c->tmo = tmo->valuedouble;
}

/* N-9 预检契约：需要交互审批时凭据生成、帧载荷平铺四键、等待窗口透传；
 * 探测不可达与无 UI 时回退「无需审批」（fail-open 继续，真实执行仍
 * fail-closed）。 */
static void test_appr_probe(void)
{
    char tmpl[] = "/tmp/airy-probe-XXXXXX";
    char *home = mkdtemp(tmpl);
    CHECK(home != NULL, "probe: temp home created");
    if (!home)
        return;
    char sockpath[256];
    snprintf(sockpath, sizeof(sockpath), "%s/probe.sock", home);

    probe_srv_t srv = {sockpath,
                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"needed\":true,"
                       "\"timeout_ms\":45000}}",
                       0};
    pthread_t st;
    CHECK(pthread_create(&st, NULL, probe_srv_run, &srv) == 0, "probe: server thread");
    for (int i = 0; i < 200 && access(sockpath, F_OK) != 0; i++)
        usleep(5000);

    appr_cap_t cap;
    AIRY_MEMSET(&cap, 0, sizeof(cap));
    agent_run_event_sink_t sink = {appr_emit, &cap};
    uint64_t seq = 0;
    run_appr_ui_t ui = {&sink, &seq, "run-9", "sess-9"};

    char rid[32];
    uint64_t wait_ms = 0;
    run_appr_probe(sockpath, "shell_run", "{\"cmd\":\"ls\"}", false, "coding_v1", &ui, rid,
                   sizeof(rid), &wait_ms);
    CHECK(rid[0] == 'a' && strlen(rid) >= 2, "probe: agent-side rid minted");
    CHECK(wait_ms == 45000, "probe: wait window forwarded");
    CHECK(cap.calls == 1 && strcmp(cap.type, AIRY_RS_TYPE_CONTROL_REQUEST) == 0,
          "probe: one control_request frame");
    CHECK(cap.rid[0] != '\0' && strcmp(cap.rid, rid) == 0, "probe: frame carries same rid");
    CHECK(strcmp(cap.tool, "shell_run") == 0, "probe: frame names the tool");
    CHECK(strcmp(cap.input, "{\"cmd\":\"ls\"}") == 0, "probe: frame carries escaped input");
    CHECK(cap.tmo == 45000, "probe: frame carries the window");

    /* 同一 ui 再次预检：凭据不重号（帧与 execute 一一对应的前提）。 */
    char rid2[32];
    uint64_t wait2 = 0;
    run_appr_probe(sockpath, "shell_run", "{}", false, NULL, &ui, rid2, sizeof(rid2), &wait2);
    CHECK(rid2[0] == 'a' && strcmp(rid2, rid) != 0, "probe: rids never repeat");

    /* 探测不可达：按「无需审批」继续，真实执行仍 fail-closed。 */
    char rid3[32];
    uint64_t wait3 = 1;
    run_appr_probe("/nonexistent/airy-probe.sock", "shell_run", "{}", false, NULL, &ui, rid3,
                   sizeof(rid3), &wait3);
    CHECK(rid3[0] == '\0' && wait3 == 0, "probe: unreachable falls back to no-approval");

    /* 无 UI：不探测、不发帧（纯执行路径与旧版时序一致）。 */
    char rid4[32];
    uint64_t wait4 = 1;
    run_appr_probe(sockpath, "shell_run", "{}", false, NULL, NULL, rid4, sizeof(rid4), &wait4);
    CHECK(rid4[0] == '\0' && wait4 == 0, "probe: NULL ui skips probing entirely");

    srv.stop = 1;
    int wfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (wfd >= 0) {
        struct sockaddr_un a;
        AIRY_MEMSET(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", sockpath);
        if (connect(wfd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            shutdown(wfd, SHUT_RDWR);
        }
        close(wfd);
    }
    pthread_join(st, NULL);
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
 * 中空输出与预算耗尽落 paused（产物已移交、可续跑）；用户取消、工具熔断与
 * 判据未过是确定性失败，落 failed；rc=0 落 completed，禁止静默停留 Executing。 */
static void test_run_state(void)
{
    CHECK(strcmp(run_state_name(0, 0), "completed") == 0, "state: rc=0 completed");
    CHECK(strcmp(run_state_name(0, 1), "failed") == 0, "state: cancel is failed");
    CHECK(strcmp(run_state_name(AGENT_RUN_RC_EMPTY, 0), "paused") == 0, "state: empty paused");
    CHECK(strcmp(run_state_name(AGENT_RUN_RC_TOOL_FUSE, 0), "failed") == 0, "state: fuse failed");
    CHECK(strcmp(run_state_name(AIRY_EBUDGET, 0), "paused") == 0, "state: ebudget paused");

    CHECK(run_rc_msg(0)[0] == '\0', "msg: success silent");
    CHECK(strstr(run_rc_msg(AGENT_RUN_RC_EMPTY), "without output") != NULL, "msg: empty named");
    CHECK(strstr(run_rc_msg(AGENT_RUN_RC_TOOL_FUSE), "fuse") != NULL, "msg: fuse named");
    CHECK(strstr(run_rc_msg(AIRY_EBUDGET), "resume_token") != NULL, "msg: ebudget resumable");
    CHECK(run_rc_msg(AGENT_RUN_RC_EMPTY) != run_rc_msg(AGENT_RUN_RC_TOOL_FUSE),
          "msg: terminal rcs distinct");

    CHECK(AGENT_RUN_RC_EMPTY == 6, "rc: empty code is 6");
}

/* D-13（E-20）：取消追溯。首个取消者为准，重复取消不得覆盖发起者与时间；
 * 未取消的会话 by 为空、ts=0；注销后按 id 取消必须落空。 */
static void test_cancel_trace(void)
{
    agent_run_session_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    const char *by = "stale";
    uint64_t ts = 4321;
    run_cancel_meta(&fresh, &by, &ts);
    CHECK(by && by[0] == '\0', "cancel meta: fresh session has empty initiator");
    CHECK(ts == 0, "cancel meta: fresh session has zero ts");

    agent_run_session_t *reg = agent_run_register("sess-cxl");
    CHECK(reg != NULL, "cancel trace: register session");
    if (!reg)
        return;
    CHECK(agent_run_cancel_by_session("sess-cxl", "cli") == AIRY_SUCCESS, "cancel: first hit");
    CHECK(agent_run_is_cancelled(reg) != 0, "cancel: flag set");

    uint64_t t1 = 0;
    run_cancel_meta(reg, &by, &t1);
    CHECK(by && strcmp(by, "cli") == 0, "cancel: initiator recorded");
    CHECK(t1 > 0, "cancel: timestamp recorded");

    CHECK(agent_run_cancel_by_session("sess-cxl", "gateway") == AIRY_SUCCESS,
          "cancel: repeat hit still succeeds");
    const char *by2 = NULL;
    run_cancel_meta(reg, &by2, &ts);
    CHECK(by2 && strcmp(by2, "cli") == 0, "cancel: first initiator wins");
    CHECK(ts == t1, "cancel: first timestamp preserved");

    agent_run_unregister(reg);
    CHECK(agent_run_cancel_by_session("sess-cxl", "cli") != AIRY_SUCCESS,
          "cancel: miss after unregister");
    CHECK(agent_run_cancel_by_session(NULL, "cli") != AIRY_SUCCESS, "cancel: null session rejected");
}

/* D-10：失败语义四类必须彼此可区分（预算/工具/审批/上游），且 retryable
 * 与 next_action 一致；分层只补字段不改原始码。 */
static void test_fail_desc(void)
{
    agent_fail_desc_t d = agent_fail_desc(AIRY_EBUDGET, 0);
    CHECK(strcmp(d.origin, AIRY_RS_ORIGIN_AGENT) == 0 &&
              strcmp(d.reason, AIRY_RS_REASON_BUDGET) == 0,
          "desc: budget named on agent");
    CHECK(d.retryable == 1 && strcmp(d.next_action, AIRY_RS_NEXT_RESUME) == 0,
          "desc: budget resumes, not resends");

    d = agent_fail_desc(AGENT_RUN_RC_TOOL_FUSE, 0);
    CHECK(strcmp(d.origin, AIRY_RS_ORIGIN_TOOL) == 0 &&
              strcmp(d.reason, AIRY_RS_REASON_TOOL) == 0,
          "desc: fuse attributed to tool");
    CHECK(d.retryable == 0 && strcmp(d.next_action, AIRY_RS_NEXT_INSPECT) == 0,
          "desc: fuse needs inspection");

    d = agent_fail_desc(AGENT_RUN_RC_TOOL_FUSE, 1);
    CHECK(strcmp(d.reason, AIRY_RS_REASON_DENIED) == 0, "desc: denial overrides fuse reason");

    d = agent_fail_desc(AGENT_RUN_RC_EMPTY, 0);
    CHECK(strcmp(d.reason, AIRY_RS_REASON_EMPTY) == 0 && d.retryable == 1 &&
              strcmp(d.next_action, AIRY_RS_NEXT_RETRY) == 0,
          "desc: empty result is safely retryable");

    d = agent_fail_desc(-1, 0);
    CHECK(strcmp(d.origin, AIRY_RS_ORIGIN_PROVIDER) == 0 &&
              strcmp(d.reason, AIRY_RS_REASON_PROVIDER) == 0 && d.retryable == 1,
          "desc: upstream failure is provider retryable");

    d = agent_fail_desc(AGENT_RUN_RC_SUBAGENT_FAIL, 0);
    CHECK(strcmp(d.origin, AIRY_RS_ORIGIN_AGENT) == 0 &&
              strcmp(d.reason, AIRY_RS_REASON_AGENT) == 0 && d.retryable == 0 &&
              strcmp(d.next_action, AIRY_RS_NEXT_INSPECT) == 0,
          "desc: unknown failure defaults to agent inspect");
}

/* D-10：审批拒绝在熔断路径上留痕，失败载荷据此与普通工具故障区分。
 * R10：拒绝属于「不可原样重发、需人工放行」，与预算（可续跑）分属两条
 * 策略，客户端只按 retryable/next_action 分流。 */
static void test_denied_flag(void)
{
    loop_ctx_t r;
    agent_fail_desc_t d;
    AIRY_MEMSET(&r, 0, sizeof(r));
    CHECK(r.denied == 0, "denied: initial state clear");
    r.denied = 1;
    d = agent_fail_desc(AGENT_RUN_RC_TOOL_FUSE, r.denied);
    CHECK(strcmp(d.origin, AIRY_RS_ORIGIN_TOOL) == 0, "denied: attributed to tool origin");
    CHECK(strcmp(d.reason, AIRY_RS_REASON_DENIED) == 0, "denied: fuse carries denial attribution");
    CHECK(d.retryable == 0, "denied: not safe to resend unchanged");
    CHECK(strcmp(d.next_action, AIRY_RS_NEXT_INSPECT) == 0, "denied: needs human approval");
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

/* base64url 编码：无填充、URL 安全字母表。 */
static void resume_check_b64(void)
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
}

/* 令牌仅含 URL 安全字母表。 */
static int resume_url_safe(const char *tok)
{
    static const char k_url_safe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (!tok)
        return 0;
    for (const char *p = tok; *p; p++) {
        if (!strchr(k_url_safe, *p))
            return 0;
    }
    return 1;
}

/* 构造 20 条工具 trace 的满预算 run 上下文。 */
static void resume_fill_ctx(loop_ctx_t *r, agent_run_loop_args_t *args,
                            agent_run_loop_result_t *out, char *prompt)
{
    AIRY_MEMSET(prompt, 'x', 4095);
    args->prompt = prompt;
    r->args = args;
    r->out = out;
    r->sess = "sess_test";
    r->trace = cJSON_CreateArray();
    for (int i = 0; i < 20; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "tool", "shell_run");
        cJSON_AddStringToObject(it, "result", "permission denied on /var/log/app.log");
        cJSON_AddNumberToObject(it, "ok", i % 2);
        cJSON_AddItemToArray(r->trace, it);
    }
    r->text = AIRY_STRDUP("halfway there");
    out->rounds_limit = 32;
    out->rounds_used = 32;
}

/* C-14：续跑令牌必须 URL 安全、无填充，且无论诉求与步骤多长都收敛到硬上限；
 * 上限内放不下时只裁剪步骤摘要，不得丢弃令牌本身。 */
static void test_resume_token(void)
{
    resume_check_b64();

    char *prompt = (char *)AIRY_CALLOC(1, 4096);
    CHECK(prompt != NULL, "resume: prompt alloc");
    if (!prompt)
        return;

    agent_run_loop_args_t args = {0};
    agent_run_loop_result_t out = {0};
    loop_ctx_t r = {0};
    resume_fill_ctx(&r, &args, &out, prompt);

    char *tok = run_resume_emit(&r);
    CHECK(tok != NULL, "resume: token emitted");
    CHECK(tok && strlen(tok) <= AGENT_RUN_RESUME_TOKEN_MAX, "resume: token within cap");
    CHECK(resume_url_safe(tok), "resume: url-safe alphabet only");
    AIRY_FREE(tok);
    cJSON_Delete(r.trace);
    AIRY_FREE(r.text);
    AIRY_FREE(prompt);
}

/* C-14：令牌必须能原样反解并重建紧凑上下文——步骤摘要与待续目标都在，
 * 但成品显著小于「原始诉求 + 全量历史」的重放，坏令牌则整体作废。 */
static void test_resume_roundtrip(void)
{
    char *prompt = AIRY_STRDUP("deploy the staging cluster");
    CHECK(prompt != NULL, "roundtrip: prompt alloc");
    if (!prompt)
        return;
    agent_run_loop_args_t args = {.prompt = prompt};
    agent_run_loop_result_t out = {0};
    loop_ctx_t r = {.args = &args, .out = &out, .sess = "sess_rt"};
    r.trace = cJSON_CreateArray();
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "tool", "shell_run");
    cJSON_AddStringToObject(it, "result", "connection timed out");
    cJSON_AddNumberToObject(it, "ok", 0);
    cJSON_AddItemToArray(r.trace, it);
    r.text = AIRY_STRDUP("next: retry via the mirror endpoint");
    out.rounds_limit = 16;
    out.rounds_used = 16;

    char *tok = run_resume_emit(&r);
    CHECK(tok != NULL, "roundtrip: token emitted");
    cJSON *p = run_resume_parse(tok ? tok : "");
    CHECK(p != NULL, "roundtrip: token parsed as JSON");
    char *ctx = p ? run_seed_render(p) : NULL;
    CHECK(ctx && strstr(ctx, "shell_run") != NULL, "roundtrip: step summary kept");
    CHECK(ctx && strstr(ctx, "connection timed out") != NULL, "roundtrip: step result kept");
    CHECK(ctx && strstr(ctx, "next: retry via the mirror endpoint") != NULL,
          "roundtrip: pending goal kept");
    CHECK(ctx && strlen(ctx) < 2048, "roundtrip: compact context, not a full replay");
    CHECK(run_resume_parse("not*base64!") == NULL, "roundtrip: corrupted token rejected");
    CHECK(run_resume_parse("") == NULL, "roundtrip: empty token rejected");

    AIRY_FREE(ctx);
    cJSON_Delete(p);
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

/* C-25：等价工具表与换路执行。注入 exec 回调验证「主工具失败后出现替代
 * 调用」，不依赖 tool_d 与 daemon 生命周期。 */
typedef struct {
    int starts;
    char tool[32];
} alt_ev_t;

static void cap_alt(const char *type, cJSON *env, void *ud)
{
    alt_ev_t *c = (alt_ev_t *)ud;
    if (strcmp(type, AIRY_RS_TYPE_TOOL_START) != 0)
        return;
    cJSON *d = env ? cJSON_GetObjectItem(env, AIRY_RS_K_DATA) : NULL;
    cJSON *t = d ? cJSON_GetObjectItem(d, AIRY_RS_K_TOOL) : NULL;
    if (cJSON_IsString(t)) {
        c->starts++;
        AIRY_STRNCPY_TERM(c->tool, t->valuestring, sizeof(c->tool));
    }
}

static struct {
    int calls;
    char tool[32];
    char args[512];
    const char *body;
    int rc;
} g_fake;

static int fake_exec(const char *name, const char *args, char **out, int *code, bool allow_dangerous,
                     airy_cancel_token_t *tok, const char *agent_id, const run_appr_ui_t *ui)
{
    (void)allow_dangerous;
    (void)tok;
    (void)agent_id;
    (void)ui;
    g_fake.calls++;
    AIRY_STRNCPY_TERM(g_fake.tool, name, sizeof(g_fake.tool));
    AIRY_STRNCPY_TERM(g_fake.args, args, sizeof(g_fake.args));
    *out = AIRY_STRDUP(g_fake.body ? g_fake.body : "");
    *code = 0;
    return g_fake.rc;
}

static void test_alt_table(void)
{
    run_alt_t a;
    int have = run_alt_plan("shell_run", "{\"command\":\"dnf install -y tree\"}", &a);
    CHECK(have && strcmp(a.tool, "priv_exec") == 0, "alt: privileged program routes to priv_exec");
    cJSON *j = have ? cJSON_Parse(a.args) : NULL;
    cJSON *p = j ? cJSON_GetObjectItem(j, "program") : NULL;
    cJSON *ar = j ? cJSON_GetObjectItem(j, "args") : NULL;
    CHECK(cJSON_IsString(p) && strcmp(p->valuestring, "dnf") == 0 && cJSON_GetArraySize(ar) == 3,
          "alt: argv preserved");
    cJSON_Delete(j);
    run_alt_free(&a);

    have = run_alt_plan("shell_run", "{\"command\":\"sudo systemctl restart sshd\"}", &a);
    CHECK(have && strcmp(a.tool, "priv_exec") == 0, "alt: sudo prefix stripped");
    run_alt_free(&a);

    CHECK(!run_alt_plan("shell_run", "{\"command\":\"grep -r foo .\"}", &a),
          "alt: non-privileged command has no equivalent");
    run_alt_free(&a);
    CHECK(!run_alt_plan("shell_run", "{\"command\":\"dnf install tree; rm -rf /\"}", &a),
          "alt: shell metachar rejects the whole command");
    run_alt_free(&a);
    CHECK(!run_alt_plan("fs_read", "{\"path\":\"/tmp/a\"}", &a),
          "alt: only shell_run has table entries");
    run_alt_free(&a);

    have = run_alt_plan("shell_run", "{\"command\":\"curl -s https://example.com/a\"}", &a);
    CHECK(have && strcmp(a.args, "{\"command\":\"wget -qO- https://example.com/a\"}") == 0,
          "alt: curl body fetch maps to wget");
    run_alt_free(&a);
    have = run_alt_plan("shell_run", "{\"command\":\"wget -qO- https://example.com/a\"}", &a);
    CHECK(have && strcmp(a.args, "{\"command\":\"curl -sSL https://example.com/a\"}") == 0,
          "alt: wget body fetch maps to curl");
    run_alt_free(&a);
    CHECK(!run_alt_plan("shell_run", "{\"command\":\"curl -X POST https://example.com/a\"}", &a),
          "alt: unsupported curl flag is not translated");
    run_alt_free(&a);
}

static void test_alt_switch(void)
{
    run_alt_t a;
    setenv("AIRY_TOOL_FALLBACK", "off", 1);
    CHECK(run_alt_enabled() == 0, "alt: AIRY_TOOL_FALLBACK=off disables");
    CHECK(run_alt_plan("shell_run", "{\"command\":\"dnf install tree\"}", &a) == 0,
          "alt: disabled table yields no plan");
    setenv("AIRY_TOOL_FALLBACK", "0", 1);
    CHECK(run_alt_enabled() == 0, "alt: 0 disables");
    unsetenv("AIRY_TOOL_FALLBACK");
    CHECK(run_alt_enabled() == 1, "alt: unset keeps enabled");
    CHECK(run_alt_deny(AIRY_EPERM) == 1 && run_alt_deny(AIRY_ERR_GENERIC_FAIL) == 0,
          "alt: policy denial blocks the swap");
}

static void test_alt_retry(void)
{
    cJSON *cat = cJSON_Parse("[{\"function\":{\"name\":\"priv_exec\"}}]");
    loop_ctx_t r;
    agent_run_loop_result_t out;
    agent_run_loop_args_t la;
    alt_ev_t ev;
    AIRY_MEMSET(&r, 0, sizeof(r));
    AIRY_MEMSET(&out, 0, sizeof(out));
    AIRY_MEMSET(&la, 0, sizeof(la));
    AIRY_MEMSET(&ev, 0, sizeof(ev));
    agent_run_event_sink_t sink = {cap_alt, &ev};
    la.sink = &sink;
    r.args = &la;
    r.out = &out;
    r.catalog = cat;

    run_alt_t alt;
    int have = run_alt_plan("shell_run", "{\"command\":\"dnf install -y tree\"}", &alt);
    AIRY_MEMSET(&g_fake, 0, sizeof(g_fake));
    g_fake.body = "tree installed";
    char *res = AIRY_STRDUP("Error: dnf: command not found");
    cJSON *fb = NULL;
    int erc = run_alt_try(&r, &alt, fake_exec, &res, &fb);
    CHECK(have && g_fake.calls == 1, "alt: substitute call executed once");
    CHECK(strcmp(g_fake.tool, "priv_exec") == 0, "alt: substitute is priv_exec");
    CHECK(erc == 0 && res && strcmp(res, "tree installed") == 0, "alt: substitute result adopted");
    CHECK(ev.starts == 1 && strcmp(ev.tool, "priv_exec") == 0, "alt: substitute emits tool_start");
    cJSON *okv = fb ? cJSON_GetObjectItem(fb, "ok") : NULL;
    CHECK(cJSON_IsNumber(okv) && okv->valueint == 1, "alt: trace fallback marks ok");
    AIRY_FREE(res);
    cJSON_Delete(fb);
    run_alt_free(&alt);

    /* 替代也失败：两段原因回填并明示表内已无替代（可解释终止）。 */
    have = run_alt_plan("shell_run", "{\"command\":\"dnf install -y tree\"}", &alt);
    g_fake.body = "denied: not root";
    g_fake.rc = -1;
    fb = NULL;
    res = AIRY_STRDUP("Error: dnf: command not found");
    erc = run_alt_try(&r, &alt, fake_exec, &res, &fb);
    CHECK(have && erc == -1, "alt: substitute failure keeps the call failed");
    CHECK(res && strstr(res, "also failed") && strstr(res, "No further equivalent tool"),
          "alt: both failures explained");
    AIRY_FREE(res);
    cJSON_Delete(fb);
    run_alt_free(&alt);
    cJSON_Delete(cat);
}

/* N-7：轮次策略与停止原因。轮次默认不设机制上限（不再夹取 32），
 * 请求值原样生效；停止原因在 done/cancelled/rounds_limit/budget 之间可判。 */
static void test_round_policy(void)
{
    CHECK(rounds_resolve(7) == 7, "policy: explicit rounds honored");
    CHECK(rounds_resolve(1000) == 1000, "policy: no mechanism hard cap");
    CHECK(rounds_resolve(0) >= 0, "policy: auto resolves non-negative");
    CHECK(tokens_resolve(500) == 500, "policy: explicit token budget honored");
    CHECK(tokens_resolve(0) > 0, "policy: default token valve is finite");

    CHECK(strcmp(run_stop_reason(0, 0, 0, 0), AIRY_RS_STOP_DONE) == 0,
          "stop: success is done");
    CHECK(strcmp(run_stop_reason(1, 1, 0, 0), AIRY_RS_STOP_CANCELLED) == 0,
          "stop: cancel is cancelled");
    CHECK(strcmp(run_stop_reason(AIRY_EBUDGET, 0, 0, 0), AIRY_RS_STOP_ROUNDS) == 0,
          "stop: explicit round cap is rounds_limit");
    CHECK(strcmp(run_stop_reason(AIRY_EBUDGET, 0, 1, 0), AIRY_RS_REASON_BUDGET) == 0,
          "stop: token valve is budget");
    CHECK(strcmp(run_stop_reason(AGENT_RUN_RC_TOOL_FUSE, 0, 0, 1), AIRY_RS_REASON_DENIED) == 0,
          "stop: denied fuse is denied");
}

int main(void)
{
    /* mock 与真实守护同为单请求单响应；对端在响应前关闭时写会收到 SIGPIPE，
     * 默认动作会直接终止进程并把用例记为 SIGPIPE 失败。写失败本身可忽略。 */
    signal(SIGPIPE, SIG_IGN);
    test_fail_streak();
    test_round_policy();
    test_emit_delta();
    test_tool_params_agent();
    test_appr_probe();
    test_stream_result_free();
    test_orch_contract();
    test_run_state();
    test_cancel_trace();
    test_fail_desc();
    test_denied_flag();
    test_switch_gate();
    test_resume_token();
    test_resume_roundtrip();
    test_fit_boundary();
    test_evt_line();
    test_alt_table();
    test_alt_switch();
    test_alt_retry();
    if (g_fail)
        printf("FAILURES: %d\n", g_fail);
    else
        printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
