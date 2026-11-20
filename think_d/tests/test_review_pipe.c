// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_review_pipe.c
 * @brief think.review pipeline 档回归：复核机制在 think_d 侧真跑（非桩）。
 *
 * 不注入 LLM 适配器时，管线必然走「门禁通过 → 两档语义均不可用 → SKIP」
 * 降级；空产物由确定性门禁直接拒绝（REJECT）。两条断言同时证明 pipeline
 * 档真的驱动了 execution_review 机制，而不是把请求原样弹回；legacy 档
 * （stage=t2）未配置模型时仍返回 -1，语义档契约不变。
 */

#include "airy_rt.h"
#include "jsonrpc_helpers.h"
#include "review_svc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

int main(void)
{
    printf("review pipeline test: skipped on Windows\n");
    return 0;
}

#else

#include <cjson/cJSON.h>
#include <sys/socket.h>
#include <unistd.h>

/* 调一次 review 方法并把响应信封解析出来（OWNER）。 */
static cJSON *pipe_call(const char *params_json, int id)
{
    cJSON *params = cJSON_Parse(params_json);
    assert(params != NULL);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    review_svc_process(params, id, &fds[0]);
    cJSON_Delete(params);
    close(fds[0]);

    char buf[4096];
    ssize_t n = read(fds[1], buf, sizeof(buf) - 1);
    close(fds[1]);
    assert(n > 0);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    assert(root != NULL);
    return root;
}

static int want_verdict(const char *params_json, int id, int want)
{
    cJSON *root = pipe_call(params_json, id);
    cJSON *res = cJSON_GetObjectItem(root, "result");
    assert(res != NULL);
    cJSON *v = cJSON_GetObjectItem(res, "verdict");
    int verdict = cJSON_IsNumber(v) ? (int)v->valuedouble : -99;
    cJSON_Delete(root);
    if (verdict != want) {
        fprintf(stderr, "FAIL: verdict=%d want=%d\n", verdict, want);
        return -1;
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    review_svc_init(NULL, NULL, NULL);

    printf("  test_pipe_degrade...\n");
    assert(want_verdict("{\"mode\":\"pipeline\",\"node_goal\":\"g\",\"output_body\":\"x\","
                        "\"output_json\":\"{\\\"text\\\":\\\"x\\\"}\"}",
                        1, AIRY_REVIEW_SKIP) == 0);

    printf("  test_pipe_empty_reject...\n");
    assert(want_verdict("{\"mode\":\"pipeline\",\"node_goal\":\"g\",\"output_body\":\"\","
                        "\"output_json\":\"\"}",
                        2, AIRY_REVIEW_REJECT) == 0);

    printf("  test_stage_t2_no_model...\n");
    assert(want_verdict("{\"stage\":\"t2\",\"output_body\":\"x\","
                        "\"output_json\":\"{\\\"text\\\":\\\"x\\\"}\"}",
                        3, -1) == 0);

    printf("  test_missing_stage...\n");
    cJSON *root = pipe_call("{\"output_body\":\"x\"}", 4);
    cJSON *err = cJSON_GetObjectItem(root, "error");
    assert(err != NULL);
    cJSON *code = cJSON_GetObjectItem(err, "code");
    assert(cJSON_IsNumber(code) && code->valueint == JSONRPC_INVALID_PARAMS);
    cJSON_Delete(root);

    printf("ALL PASSED\n");
    return 0;
}

#endif /* !_WIN32 */
