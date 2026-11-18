// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_tool_job.c
 * @brief C-11: 作业句柄核心状态机验收（提交/轮询/取消/超时夹取/JSON）。
 */

#include "job/tool_job.h"
#include "error.h"
#include "io.h"
#include "tool_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_delay_ms;

static void job_dir(char *buf, size_t cap, const char *tag)
{
    snprintf(buf, cap, "/tmp/airy-tool-job-%s-%d", tag, (int)getpid());
    airy_io_remove_dir_recursive(buf);
}

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
    if (strcmp(req->tool_id, "slow") == 0) {
        res->exit_code = -1;
        res->err_code = AIRY_ETIMEDOUT;
        free(res->output);
        res->output = strdup("partial");
        res->error = strdup("Command timed out");
    }
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
    char dir[128];
    job_dir(dir, sizeof(dir), "state");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
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
    airy_io_remove_dir_recursive(dir);
}

static void test_cancel(void)
{
    char dir[128];
    job_dir(dir, sizeof(dir), "cancel");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
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
    airy_io_remove_dir_recursive(dir);
}

/* 落盘 → 重开同目录 → 终态可读回；残留 running 记录纠正为失败。 */
static void test_persist(void)
{
    char dir[128];
    char path[256];
    job_dir(dir, sizeof(dir), "persist");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
    tool_execute_request_t req = {0};
    char id[TOOL_JOB_ID_MAX];
    tool_job_info_t info;

    assert(jobs != NULL);
    g_delay_ms = 0;
    req.tool_id = "fake";
    req.params_json = "{}";
    assert(tooljob_submit(jobs, &req, 20, id, sizeof(id)) == 0);
    wait_state(jobs, id, TOOL_JOB_DONE);
    tooljob_free(jobs);

    jobs = tooljob_new(dir, fake_run, NULL);
    assert(jobs != NULL);
    assert(tooljob_count(jobs) == 1);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.state == TOOL_JOB_DONE);
    assert(info.output && strcmp(info.output, "hello") == 0);
    assert(info.timeout_sec == 20);
    tooljob_rel(&info);
    tooljob_free(jobs);

    /* 模拟守护进程在作业运行中被杀：磁盘上留下 running 记录。 */
    snprintf(path, sizeof(path), "%s/job-crash.json", dir);
    assert(airy_io_write_file(path,
                              "{\"job_id\":\"job-crash\",\"tool_id\":\"fake\","
                              "\"agent_id\":\"\",\"state\":\"running\","
                              "\"created_ms\":1,\"finished_ms\":0}",
                              (size_t)-1) == 0);
    jobs = tooljob_new(dir, fake_run, NULL);
    assert(jobs != NULL);
    assert(tooljob_count(jobs) == 2);
    assert(tooljob_poll(jobs, "job-crash", &info) == 0);
    assert(info.state == TOOL_JOB_FAILED);
    assert(info.error && strstr(info.error, "restarted") != NULL);
    tooljob_rel(&info);
    tooljob_free(jobs);

    /* 纠正后的记录已回写，再次加载仍是失败态。 */
    jobs = tooljob_new(dir, fake_run, NULL);
    assert(jobs != NULL);
    assert(tooljob_poll(jobs, "job-crash", &info) == 0);
    assert(info.state == TOOL_JOB_FAILED);
    tooljob_rel(&info);
    tooljob_free(jobs);
    airy_io_remove_dir_recursive(dir);
}

static void test_errors(void)
{
    char dir[128];
    job_dir(dir, sizeof(dir), "errors");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
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

    /* 超时结果 → timed_out，且保留执行层已捕获的部分输出。 */
    req.tool_id = "slow";
    assert(tooljob_submit(jobs, &req, 0, id, sizeof(id)) == 0);
    wait_state(jobs, id, TOOL_JOB_TIMED_OUT);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.err_code == AIRY_ETIMEDOUT);
    assert(info.output && strcmp(info.output, "partial") == 0);
    tooljob_rel(&info);

    assert(tooljob_clamp(-5) == 0);
    assert(tooljob_clamp(30) == 30);
    assert(tooljob_clamp(100000) == 600);
    tooljob_free(jobs);
    airy_io_remove_dir_recursive(dir);
}

/* D-9：提交即写脱敏参数与风险，收尾写输出摘要/审批/沙箱，重开可读回。 */
static void test_audit_record(void)
{
    char dir[128];
    job_dir(dir, sizeof(dir), "audit");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
    tool_execute_request_t req = {0};
    char id[TOOL_JOB_ID_MAX];
    tool_job_info_t info;

    assert(jobs != NULL);
    g_delay_ms = 0;
    req.tool_id = "shell_run";
    req.params_json = "{\"command\":\"echo hi\",\"password\":\"s3cr3t\"}";
    req.allow_dangerous = 1;
    assert(tooljob_submit(jobs, &req, 20, id, sizeof(id)) == 0);
    wait_state(jobs, id, TOOL_JOB_DONE);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.risk == 1);
    assert(strstr(info.risk_reason, "shell_run") != NULL);
    assert(strstr(info.params_sum, "s3cr3t") == NULL);
    assert(strstr(info.params_sum, "***") != NULL);
    assert(strstr(info.params_sum, "echo hi") != NULL);
    assert(info.output_sum && strcmp(info.output_sum, "hello") == 0);
    assert(strcmp(info.approval, "allow_dangerous") == 0);
    assert(info.sandbox[0] != '\0');
    tooljob_rel(&info);
    tooljob_free(jobs);

    /* 审计字段必须随记录落盘，重启后仍可还原时间线。 */
    jobs = tooljob_new(dir, fake_run, NULL);
    assert(jobs != NULL);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(info.risk == 1);
    assert(strstr(info.params_sum, "s3cr3t") == NULL);
    assert(strcmp(info.approval, "allow_dangerous") == 0);
    assert(info.sandbox[0] != '\0');
    tooljob_rel(&info);
    tooljob_free(jobs);
    airy_io_remove_dir_recursive(dir);
}

/* D-9：同步执行落账（tool.execute 路径）与后台作业共用同一实体。 */
static void test_record(void)
{
    char dir[128];
    job_dir(dir, sizeof(dir), "record");
    tooljob_t *jobs = tooljob_new(dir, fake_run, NULL);
    tool_result_t res;
    char id[TOOL_JOB_ID_MAX];
    tool_job_info_t info;

    assert(jobs != NULL);
    memset(&res, 0, sizeof(res));
    res.exit_code = 0;
    res.err_code = 0;
    res.output = strdup("apt-get install -y curl: done");
    snprintf(res.approval, sizeof(res.approval), "allow_dangerous");
    snprintf(res.sandbox, sizeof(res.sandbox), "landlock");
    assert(tooljob_record(jobs, "pkg_install", "agent-1",
                          "{\"command\":\"apt-get install -y curl\",\"password\":\"p\"}", &res,
                          1000, id, sizeof(id)) == 0);

    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(strcmp(info.tool_id, "pkg_install") == 0);
    assert(strcmp(info.agent_id, "agent-1") == 0);
    assert(info.state == TOOL_JOB_DONE);
    assert(info.created_ms == 1000);
    assert(info.finished_ms >= info.created_ms);
    assert(info.risk == 1); /* 包管理命令模式 → 高危 */
    assert(strstr(info.params_sum, "\"p\"") == NULL);
    assert(strstr(info.params_sum, "password") != NULL);
    assert(strstr(info.params_sum, "apt-get") != NULL);
    assert(strcmp(info.approval, "allow_dangerous") == 0);
    assert(strcmp(info.sandbox, "landlock") == 0);
    assert(info.output_sum && strstr(info.output_sum, "apt-get") != NULL);
    tooljob_rel(&info);

    assert(tooljob_record(jobs, NULL, "agent-1", "{}", &res, 0, id, sizeof(id)) ==
           AIRY_ERR_INVALID_PARAM);
    free(res.output);
    tooljob_free(jobs);

    /* 同步记录同样落盘，重启后仍可查。 */
    jobs = tooljob_new(dir, fake_run, NULL);
    assert(jobs != NULL);
    assert(tooljob_poll(jobs, id, &info) == 0);
    assert(strcmp(info.approval, "allow_dangerous") == 0);
    tooljob_rel(&info);
    tooljob_free(jobs);
    airy_io_remove_dir_recursive(dir);
}

int main(void)
{
    test_submit_poll();
    test_cancel();
    test_persist();
    test_errors();
    test_audit_record();
    test_record();
    printf("test_tool_job: all passed\n");
    return 0;
}
