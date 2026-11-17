// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_task_ledger.c
 * @brief 任务事件账本主键与四类事件的落盘单测（0.1.21 C-18）。
 *
 * 只验证真实写入面（hall_event 的目录/文件名/信封格式），不 stub：
 *  - task 提交以 task_id 为主键，chat 回退 session_id；
 *  - blueprint/progress/issue/result 分别落到对应 category 目录；
 *  - 事件 content 携带 event/state/reason/rc 等字段级证据；
 *  - 空主键拒绝写入（不产生无主目录）。
 */

#include "run/agent_task_contract.h"
#include "run/agent_task_ledger.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/* 取目录内字典序最大的事件文件（ts_utc.seq 命名，最大即最新）。 */
static int latest_event(const char *dir, char *out, size_t out_sz)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    out[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0)
            continue;
        if (out[0] == '\0' || strcmp(e->d_name, out) > 0)
            snprintf(out, out_sz, "%s", e->d_name);
    }
    closedir(d);
    return out[0] ? 0 : -1;
}

static int count_events(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".json") == 0)
            n++;
    }
    closedir(d);
    return n;
}

/* 读取事件全文；调用方负责 free。 */
static char *read_event(const char *dir)
{
    char name[256];
    if (latest_event(dir, name, sizeof(name)) != 0)
        return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)calloc(1, 8192);
    if (buf)
        (void)fread(buf, 1, 8191, f);
    fclose(f);
    return buf;
}

int main(void)
{
    char home[256];
    snprintf(home, sizeof(home), "/tmp/agent_d_task_ledger_%ld", (long)getpid());
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
    if (system(cmd) != 0)
        return 1;
    setenv("AIRY_HOME", home, 1);

    agent_run_task_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.task_id, sizeof(meta.task_id), "task-ledger-1");
    snprintf(meta.plan_id, sizeof(meta.plan_id), "plan-ledger-1");

    /* 1. 主键选择：task_id 优先，chat 回退 session，双空返回 NULL。 */
    CHECK(agent_task_ledger_key(&meta, "sess-x") == meta.task_id, "key must prefer task_id");
    CHECK(agent_task_ledger_key(NULL, "sess-x") != NULL &&
              strcmp(agent_task_ledger_key(NULL, "sess-x"), "sess-x") == 0,
          "key must fall back to session_id");
    CHECK(agent_task_ledger_key(NULL, NULL) == NULL, "empty key must be NULL");

    char base[320];
    snprintf(base, sizeof(base), "%s/data/agentrt/hall/default/task-ledger-1", home);
    char dir[400];
    char *body;

    /* 2. blueprint：目标与成功判据在提交时冻结。 */
    CHECK(agent_task_ledger_blueprint(meta.task_id, meta.plan_id, "build the hall",
                                      "board shows terminal") == 0,
          "blueprint write failed");
    snprintf(dir, sizeof(dir), "%s/blueprint", base);
    body = read_event(dir);
    CHECK(body && strstr(body, "\"event\":\"blueprint\""), "blueprint event missing");
    CHECK(body && strstr(body, "build the hall"), "blueprint goal missing");
    CHECK(body && strstr(body, "\"task_id\":\"task-ledger-1\""), "blueprint task_id missing");
    free(body);

    /* 3. progress：过程态落 progress，终态落 result。 */
    CHECK(agent_task_ledger_progress(meta.task_id, meta.plan_id, "submitted", NULL) == 0,
          "progress(submitted) failed");
    CHECK(agent_task_ledger_progress(meta.task_id, meta.plan_id, "planned", NULL) == 0,
          "progress(planned) failed");
    CHECK(agent_task_ledger_progress(meta.task_id, meta.plan_id, "executing", NULL) == 0,
          "progress(executing) failed");
    snprintf(dir, sizeof(dir), "%s/progress", base);
    CHECK(count_events(dir) == 3, "progress count=%d want=3", count_events(dir));
    body = read_event(dir);
    CHECK(body && strstr(body, "\"state\":\"executing\""), "latest progress state wrong");
    CHECK(body && strstr(body, "\"progress\":0.5"), "progress ratio missing");
    free(body);

    /* 4. issue：失败/取消必须留下 rc + reason。 */
    CHECK(agent_task_ledger_issue(meta.task_id, -5, "tool loop exhausted") == 0,
          "issue write failed");
    snprintf(dir, sizeof(dir), "%s/issue", base);
    body = read_event(dir);
    CHECK(body && strstr(body, "\"event\":\"issue\""), "issue event missing");
    CHECK(body && strstr(body, "\"rc\":-5"), "issue rc missing");
    CHECK(body && strstr(body, "tool loop exhausted"), "issue reason missing");
    free(body);

    /* 5. result：终态事件在原目录同源落盘。 */
    CHECK(agent_task_ledger_result(meta.task_id, meta.plan_id, "completed") == 0,
          "result write failed");
    snprintf(dir, sizeof(dir), "%s/result", base);
    body = read_event(dir);
    CHECK(body && strstr(body, "\"state\":\"completed\""), "terminal state missing");
    CHECK(body && strstr(body, "\"progress\":1"), "terminal progress wrong");
    free(body);

    /* 6. 空主键不产生无主目录。 */
    CHECK(agent_task_ledger_result(NULL, NULL, "failed") != 0, "empty key must be rejected");
    CHECK(agent_task_ledger_progress("", NULL, "executing", NULL) != 0,
          "empty key must be rejected");

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
    if (system(cmd) != 0)
        return 1;

    if (g_failures) {
        printf("task_ledger: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("task_ledger: ok\n");
    return 0;
}
