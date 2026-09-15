// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_pool_e2e.c
 * @brief R1-a/C-6 (0.1.17): 有界池端到端验收（经 tool_service_execute 全链）。
 *
 * 与 test_executor_pool.c（池内单测，伪造 executor）互补：本测试走生产
 * 入口 tool_service_execute 的完整链路（注册表查找 → 参数校验 → ACL →
 * 缓存检查 → executor_pool → executor → 真实 builtin），使用真实注册
 * 工具 shell_run（慢）与 fs_write（快）：
 *   1. 两会话并发：慢会话 shell_run "sleep 2" 超预算（1500ms）被取消；
 *   2. 快会话 fs_write 不受影响，先于慢会话取消点完成；
 *   3. 取消返回 AIRY_ERR_CANCELED + 合成 NORMAL_FAIL 结果
 *      （success=0 / error 含 canceled / exit_code=-1）。
 */

#include "daemon_security.h"
#include "error.h"
#include "tool_service.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CHECK
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #cond);                                      \
            abort();                                                       \
        }                                                                  \
    } while (0)
#endif

static int g_checks = 0;
static int g_fails = 0;

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

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(long ms)
{
    struct timespec req = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&req, NULL);
}

typedef struct {
    tool_service_t *svc;
    const char *tool_id;
    const char *params_json;
    tool_result_t *res;
    int ret;
    long long start_ms;
    long long end_ms;
} svc_thread_arg_t;

static void *slow_thread_fn(void *argp)
{
    svc_thread_arg_t *arg = (svc_thread_arg_t *)argp;
    tool_execute_request_t req;
    memset(&req, 0, sizeof(req));
    req.tool_id = arg->tool_id;
    req.params_json = arg->params_json;
    req.agent_id = "tool_d";

    arg->start_ms = now_ms();
    arg->res = NULL;
    arg->ret = tool_service_execute(arg->svc, &req, &arg->res);
    arg->end_ms = now_ms();
    return NULL;
}

int main(void)
{
    printf("=== C-6/R1-a: 有界池端到端（tool_service_execute 全链）===\n\n");

    /* CI-2：PID 隔离，避免 ctest -j 并行互踩 */
    char ws[256];
    char fast_path[320];
    snprintf(ws, sizeof(ws), "/tmp/airy_pool_e2e_%d", (int)getpid());
    snprintf(fast_path, sizeof(fast_path), "%s/fast_ok.txt", ws);
    CHECK(mkdir(ws, 0755) == 0);

    /* T16: fs 工具 workspace 围堵；R1-a: 全局等待预算 1500ms */
    setenv("AIRY_TOOL_SANDBOX_WORKSPACE", ws, 1);
    setenv("AIRY_TOOL_WAIT_BUDGET_MS", "1500", 1);
    unsetenv("AIRY_TOOL_EXEC_WORKERS");

    CHECK(daemon_security_init(NULL, NULL) == 0);
    CHECK(daemon_security_add_acl_rule("tool_d", "shell_run", true) == 0);
    CHECK(daemon_security_add_acl_rule("tool_d", "fs_write", true) == 0);

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    /* 慢会话：真实 shell_run "sleep 2"（2000ms > 1500ms 预算 → 取消） */
    svc_thread_arg_t slow;
    memset(&slow, 0, sizeof(slow));
    slow.svc = svc;
    slow.tool_id = "shell_run";
    slow.params_json = "{\"command\":\"sleep 2\"}";

    pthread_t th;
    CHECK(pthread_create(&th, NULL, slow_thread_fn, &slow) == 0);

    /* 确保慢会话已入池在途，再注入快会话（独立 worker） */
    nap_ms(200);

    long long fast_start = now_ms();
    cJSON *p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", fast_path);
    cJSON_AddStringToObject(p, "content", "pool-e2e-ok");
    char *fast_json = cJSON_PrintUnformatted(p);
    CHECK(fast_json != NULL);
    cJSON_Delete(p);

    tool_execute_request_t fast_req;
    memset(&fast_req, 0, sizeof(fast_req));
    fast_req.tool_id = "fs_write";
    fast_req.params_json = fast_json;
    fast_req.agent_id = "tool_d";

    tool_result_t *fast_res = NULL;
    int fast_ret = tool_service_execute(svc, &fast_req, &fast_res);
    long long fast_end = now_ms();
    free(fast_json);

    pthread_join(th, NULL);

    /* 1. 快会话：正常完成（隔离性） */
    TEST(fast_ret == AIRY_OK && fast_res && fast_res->success == 1,
         "快会话 fs_write 正常完成");
    if (fast_res) {
        tool_result_free(fast_res);
    }

    /* 2. 快会话产物真实落地 */
    FILE *fp = fopen(fast_path, "r");
    char buf[32] = {0};
    int file_ok = fp && fread(buf, 1, sizeof(buf) - 1, fp) > 0 &&
                  strcmp(buf, "pool-e2e-ok") == 0;
    if (fp) {
        fclose(fp);
    }
    TEST(file_ok, "fs_write 产物内容正确");

    /* 3. 快会话远小于预算完成（200ms 错峰 + 执行耗时） */
    TEST((fast_end - fast_start) < 1000, "快会话远小于预算（<1000ms）");

    /* 4. 隔离时序：快会话先于慢会话取消点完成 */
    TEST(fast_end < slow.end_ms, "快会话先于慢会话取消点完成");

    /* 5. 慢会话：明确取消错误码 */
    TEST(slow.ret == AIRY_ERR_CANCELED, "慢会话返回 AIRY_ERR_CANCELED");

    /* 6. 合成取消结果语义 */
    TEST(slow.res && slow.res->success == 0 &&
             slow.res->error && strstr(slow.res->error, "cancel") != NULL &&
             slow.res->exit_code == -1 &&
             slow.res->failure_class == TOOL_RESULT_CLASS_NORMAL_FAIL,
         "合成结果：NORMAL_FAIL + canceled + exit_code=-1");

    /* 7. 取消时机在预算附近（非提前返回、非 sleep 自然完成） */
    long long slow_ms = slow.end_ms - slow.start_ms;
    TEST(slow_ms >= 1000 && slow_ms < 5000, "取消发生在等待预算附近");
    printf("    时序：fast %lldms / slow %lldms（预算 1500ms）\n",
           fast_end - fast_start, slow_ms);

    if (slow.res) {
        tool_result_free(slow.res);
    }

    tool_service_destroy(svc); /* drain 在途 detach job（sleep 残段） */
    unlink(fast_path);
    rmdir(ws);

    printf("\n=== 结果：%d/%d 通过 ===\n", g_checks - g_fails, g_checks);
    return g_fails == 0 ? 0 : 1;
}
