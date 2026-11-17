// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_tool_job.c
 * @brief C-11: 作业句柄核心状态机验收（提交/轮询/取消/超时夹取/JSON）。
 */

#include "job/tool_job.h"
#include "error.h"
#include "tool_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_delay_ms;

static int fake_run(void *ctx, const tool_execute_request_t *req, tool_result_t **out)
{
    (void)ctx;
    usleep((useconds_t)g_delay_ms * 1000);
    if (!req || strcmp(req->tool_id, "boom") == 0)
        return AIRY_ERR_GENERIC_FAIL;
    tool_result_t *res = calloc(1, sizeof(*res));
    assert(res != NULL);
    res->exit_code = 0;
    res->err_code = 0;
    res->output = strdup("hello");
    *out = res;
    return 0;
}

static void wait_state(tooljob_t *jobs, const char *id, tool_job_state_t want)
{
    tool_job_info_t info;
    for (int i = 0; i < 200; i++) {
        assert(tooljob_poll(jobs, id, &info) == 0);
        tool_job_state_t st = info.state;
        tooljob_rel(&info);
        if (st == want)
            return;
        usleep(10 * 1000);
    }
    assert(!"job did not reach expected state");
}

static void test_submit_poll(void)
{
    tooljob_t *jobs = tooljob_new("/tmp/airy-tool-job-test", fake_run, NULL);
    tool_execute_request_t req = {0};
    char id[TOOL_JOB_ID_MAX];

    assert(jobs != NULL);
    g_delay_ms = 60;
    req.tool_id = "fake";
    req.params_json = "{}";
    assert(tooljob_submit(jobs, &req, 30, id, sizeof(id)) == 0);
    assert(strncmp(id, "job-", 4) == 0);

    tool_job_info_t info;
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.state == TOOL_JOB_RUNNING);
    assert(info.timeout_sec == 30);
    tooljob_rel(&info);

    wait_state(jobs, id, TOOL_JOB_DONE);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.output && strcmp(info.output, "hello") == 0);
    assert(info.err_code == 0);
    cJSON *wire = tooljob_json(&info);
    assert(wire != NULL);
    assert(strcmp(cJSON_GetObjectItem(wire, "state")->valuestring, "done") == 0);
    cJSON_Delete(wire);
    tooljob_rel(&info);
    tooljob_free(jobs);
}

static void test_cancel(void)
{
    tooljob_t *jobs = tooljob_new("/tmp/airy-tool-job-test", fake_run, NULL);
    tool_execute_request_t req = {0};
    char id[TOOL_JOB_ID_MAX];

    g_delay_ms = 300;
    req.tool_id = "fake";
    req.params_json = "{}";
    assert(tooljob_submit(jobs, &req, 60, id, sizeof(id)) == 0);
    assert(tooljob_cancel(jobs, id) == 0);

    tool_job_info_t info;
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.state == TOOL_JOB_CANCELLED);
    assert(info.output == NULL);
    tooljob_rel(&info);

    /* 协作取消：执行线程收尾后不得把结果写回终态。 */
    usleep(400 * 1000);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.state == TOOL_JOB_CANCELLED);
    assert(info.output == NULL);
    tooljob_rel(&info);
    assert(tooljob_cancel(jobs, id) == AIRY_ERR_STATE_ERROR);
    tooljob_free(jobs);
}

static void test_errors(void)
{
    tooljob_t *jobs = tooljob_new("/tmp/airy-tool-job-test", fake_run, NULL);
    tool_execute_request_t req = {0};
    char id[TOOL_JOB_ID_MAX];
    tool_job_info_t info;

    assert(tooljob_poll(jobs, "job-nope", &info) == AIRY_ERR_NOT_FOUND);
    assert(tooljob_cancel(jobs, "job-nope") == AIRY_ERR_NOT_FOUND);
    assert(tooljob_submit(jobs, &req, 0, id, sizeof(id)) == AIRY_ERR_INVALID_PARAM);

    /* 执行层拒绝（无结果对象）→ failed 且带错误码。 */
    g_delay_ms = 0;
    req.tool_id = "boom";
    req.params_json = "{}";
    assert(tooljob_submit(jobs, &req, 0, id, sizeof(id)) == 0);
    wait_state(jobs, id, TOOL_JOB_FAILED);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.err_code == AIRY_ERR_GENERIC_FAIL);
    assert(info.error != NULL);
    tooljob_rel(&info);

    assert(tooljob_clamp(-5) == 0);
    assert(tooljob_clamp(30) == 30);
    assert(tooljob_clamp(100000) == 600);
    tooljob_free(jobs);
}

int main(void)
{
    test_submit_poll();
    test_cancel();
    test_errors();
    printf("test_tool_job: all passed\n");
    return 0;
}
