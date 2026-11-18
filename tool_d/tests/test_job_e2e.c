// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_job_e2e.c
 * @brief C-11 (0.1.21): 作业句柄端到端——submit → poll → 终态取回完整输出。
 *
 * 与 test_tool_job.c（伪造执行回调，只验状态机）互补：本测试的执行回调就是
 * 生产入口 tool_service_execute，链路为 作业线程 → 注册表 → ACL → 有界池 →
 * executor → 真实 builtin shell_run（/bin/sh 子进程）。覆盖：
 *   1. 长命令：submit 立即返回且 poll 可见 running，完成后取回完整 stdout；
 *   2. 落盘：重开同目录作业存储，终态与输出仍可读回；
 *   3. 超时：执行层 SIGKILL 后映射 timed_out，保留已捕获的部分输出；
 *   4. 取消：运行中 cancel 立即转 cancelled，且不被执行线程覆盖。
 *
 * 长命令时长默认 3s；设 AIRY_JOB_E2E_LONG_MS=65000 可复跑 gate:lab 的
 * 「>60s 命令」判据（一次实跑约 65s，不进默认 ctest 时长）。
 */

#include "daemon_security.h"
#include "error.h"
#include "io.h"
#include "job/tool_job.h"
#include "platform.h"
#include "tool_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks;
static int g_fails;

#define TEST(cond, name)                          \
    do {                                          \
        g_checks++;                               \
        if (cond) {                               \
            printf("  [PASS] %s\n", name);        \
        } else {                                  \
            g_fails++;                            \
            printf("  [FAIL] %s (%s:%d)\n", name, \
                   __FILE__, __LINE__);           \
        }                                         \
    } while (0)

static int svc_run(void *ctx, const tool_execute_request_t *req, tool_result_t **out)
{
    return tool_service_execute((tool_service_t *)ctx, req, out);
}

/* 轮询到终态；超时返回当前快照（仍为 running）由调用方断言。 */
static int wait_final(tooljob_t *jobs, const char *id, tool_job_info_t *out, int budget_ms)
{
    int waited = 0;

    for (;;) {
        if (tooljob_poll(jobs, id, out) != 0)
            return -1;
        if (out->state != TOOL_JOB_RUNNING)
            return 0;
        if (waited >= budget_ms)
            return 0;
        airy_sleep_ms(50);
        waited += 50;
    }
}

static int submit(tooljob_t *jobs, const char *cmd, int timeout_sec, char *id, size_t cap)
{
    tool_execute_request_t req;
    char params[512];

    snprintf(params, sizeof(params), "{\"command\":\"%s\"}", cmd);
    memset(&req, 0, sizeof(req));
    req.tool_id = "shell_run";
    req.params_json = params;
    req.agent_id = "tool_d";
    return tooljob_submit(jobs, &req, timeout_sec, id, cap);
}

int main(void)
{
    char ws[256];
    char root[320];
    char id[TOOL_JOB_ID_MAX];
    const char *env = getenv("AIRY_JOB_E2E_LONG_MS");
    int long_ms = env && atoi(env) > 0 ? atoi(env) : 3000;
    char cmd[128];
    tool_job_info_t info;

    printf("=== C-11: 作业句柄端到端（tool_service_execute 全链，长命令 %dms）===\n\n",
           long_ms);

    snprintf(ws, sizeof(ws), "/tmp/airy_job_e2e_%d", (int)getpid());
    snprintf(root, sizeof(root), "%s/jobs", ws);
    if (mkdir(ws, 0755) != 0) {
        fprintf(stderr, "mkdir %s failed\n", ws);
        return 1;
    }
    setenv("AIRY_HOME", ws, 1);
    setenv("AIRY_DATA_DIR", "", 1);
    setenv("AIRY_TOOL_SANDBOX_WORKSPACE", ws, 1);
    unsetenv("AIRY_TOOL_WAIT_BUDGET_MS");

    if (daemon_security_init(NULL, NULL) != 0 ||
        daemon_security_add_acl_rule("tool_d", "shell_run", true) != 0) {
        fprintf(stderr, "daemon_security init failed\n");
        return 1;
    }

    tool_service_t *svc = tool_service_create(NULL);
    tooljob_t *jobs = svc ? tooljob_new(root, svc_run, svc) : NULL;
    if (!jobs) {
        fprintf(stderr, "service/job store init failed\n");
        tool_service_destroy(svc);
        return 1;
    }

    /* 1. 长命令：submit 返回 running，完成后取回完整 stdout。 */
    snprintf(cmd, sizeof(cmd), "echo begin; sleep %d.%03d; echo end",
             long_ms / 1000, long_ms % 1000);
    TEST(submit(jobs, cmd, 120, id, sizeof(id)) == 0, "长命令 submit 成功");
    TEST(tooljob_poll(jobs, id, &info) == 0 && info.state == TOOL_JOB_RUNNING,
         "提交后立即 poll 为 running");
    tooljob_rel(&info);
    TEST(wait_final(jobs, id, &info, long_ms + 30000) == 0 && info.state == TOOL_JOB_DONE,
         "长命令轮询到 done");
    TEST(info.err_code == 0 && info.exit_code == 0, "长命令 err_code/exit_code 均为 0");
    TEST(info.output && strstr(info.output, "begin") && strstr(info.output, "end"),
         "取回完整 stdout（含首尾标记）");
    printf("    输出：%.*s\n", info.output ? (int)strcspn(info.output, "\n") : 0,
           info.output ? info.output : "");
    tooljob_rel(&info);

    /* 2. 落盘：重开同目录，终态与输出仍可读回。 */
    tooljob_t *reopened = tooljob_new(root, svc_run, svc);
    TEST(reopened != NULL, "重开作业存储成功");
    if (reopened) {
        TEST(tooljob_poll(reopened, id, &info) == 0 && info.state == TOOL_JOB_DONE,
             "重启恢复：终态仍为 done");
        TEST(info.output && strstr(info.output, "end"), "重启恢复：输出完整");
        tooljob_rel(&info);
        tooljob_free(reopened);
    }

    /* 3. 超时：执行层 SIGKILL 后映射 timed_out，保留部分输出。 */
    TEST(submit(jobs, "echo partial; sleep 20", 1, id, sizeof(id)) == 0, "超时用例 submit 成功");
    TEST(wait_final(jobs, id, &info, 30000) == 0 && info.state == TOOL_JOB_TIMED_OUT,
         "超时映射为 timed_out");
    TEST(info.err_code != 0 && info.output && strstr(info.output, "partial"),
         "超时保留已捕获的部分输出");
    tooljob_rel(&info);

    /* 4. 取消：运行中 cancel 立即生效，执行线程收尾不覆盖终态。 */
    TEST(submit(jobs, "sleep 10", 60, id, sizeof(id)) == 0, "取消用例 submit 成功");
    TEST(tooljob_cancel(jobs, id) == 0, "运行中 cancel 成功");
    TEST(tooljob_poll(jobs, id, &info) == 0 && info.state == TOOL_JOB_CANCELLED,
         "取消后状态为 cancelled");
    tooljob_rel(&info);
    airy_sleep_ms(300);
    TEST(tooljob_poll(jobs, id, &info) == 0 && info.state == TOOL_JOB_CANCELLED,
         "执行线程收尾后仍为 cancelled");
    tooljob_rel(&info);

    TEST(tooljob_drain(jobs, 30000) == 0, "drain 等待在途作业收尾");
    tooljob_free(jobs);
    tool_service_destroy(svc);
    {
        char path[400];
        snprintf(path, sizeof(path), "%s", root);
        airy_io_remove_dir_recursive(path);
    }
    rmdir(ws);

    printf("\n=== 结果：%d/%d 通过 ===\n", g_checks - g_fails, g_checks);
    return g_fails == 0 ? 0 : 1;
}
