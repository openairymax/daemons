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
#include "hall_event.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/* 读取任意事件文件全文；调用方负责 free。 */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)calloc(1, 8192);
    if (buf)
        (void)fread(buf, 1, 8191, f);
    fclose(f);
    return buf;
}

/* 读取目录内最新事件全文；调用方负责 free。 */
static char *read_event(const char *dir)
{
    char name[256];
    if (latest_event(dir, name, sizeof(name)) != 0)
        return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return read_file(path);
}

/* ── 重启回放（C-18-07 实现段 6/6）─────────────────────────────────
 * 账本是 append-only 磁盘事件：进程重启后内存态全部丢弃，回放只靠
 * 目录遍历 + (ts_utc, gseq) 排序。子进程经 exec 重建（不是 fork 后
 * 复用内存），因此这里证明的是真实跨进程续接，而非同进程重读。
 */

#define REPLAY_MAX_ROWS 64

typedef struct {
    char ts[HALL_EVT_TS_LEN];
    unsigned long long gseq;
    char token[96]; /* "category" 或 "category:state" */
} replay_row_t;

static int replay_row_cmp(const void *a, const void *b)
{
    const replay_row_t *ra = (const replay_row_t *)a;
    const replay_row_t *rb = (const replay_row_t *)b;
    int c = strcmp(ra->ts, rb->ts);
    if (c != 0)
        return c;
    return (ra->gseq > rb->gseq) - (ra->gseq < rb->gseq);
}

/* 把事件归一为可比较的类别记号：progress/result 附带 content.state。 */
static void replay_token(const char *category, const char *body, char *out, size_t sz)
{
    const char *tag = "\"state\":\"";
    const char *p = body ? strstr(body, tag) : NULL;
    if (!p) {
        snprintf(out, sz, "%s", category);
        return;
    }
    p += strlen(tag);
    size_t i = 0;
    while (p[i] && p[i] != '"' && i + 2 < sz)
        i++;
    snprintf(out, sz, "%s:%.*s", category, (int)i, p);
}

static int scan_events(const char *base, replay_row_t *rows, int cap)
{
    static const char *cats[] = {"blueprint", "progress", "issue", "result"};
    int n = 0;
    for (size_t c = 0; c < sizeof(cats) / sizeof(cats[0]); c++) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/%s", base, cats[c]);
        DIR *d = opendir(dir);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < cap) {
            size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            hall_evt_parts_t parts;
            if (hall_evt_parse(e->d_name, &parts) != 0)
                continue;
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            char *body = read_file(path);
            if (!body)
                continue;
            replay_row_t *r = &rows[n++];
            snprintf(r->ts, sizeof(r->ts), "%.*s", (int)parts.ts_utc_len, parts.ts_utc);
            r->gseq = hall_evt_gseq_of(body);
            replay_token(cats[c], body, r->token, sizeof(r->token));
            free(body);
        }
        closedir(d);
    }
    return n;
}

/* 子进程：不持有父进程任何内存态，只凭 $AIRY_HOME 的磁盘账本回放。 */
static void replay_check(const char *home)
{
    char base[320];
    snprintf(base, sizeof(base), "%s/data/agentrt/hall/default/task-ledger-1", home);

    replay_row_t rows[REPLAY_MAX_ROWS];
    int n = scan_events(base, rows, REPLAY_MAX_ROWS);
    CHECK(n == 6, "restart replay saw %d events, want 6 (blueprint+3 progress+issue+result)", n);
    qsort(rows, (size_t)n, sizeof(rows[0]), replay_row_cmp);

    static const char *want[] = {"blueprint", "progress:submitted", "progress:planned",
                                 "progress:executing", "issue", "result:completed"};
    for (int i = 0; i < n && i < 6; i++)
        CHECK(strcmp(rows[i].token, want[i]) == 0, "replay[%d]=%s want=%s", i, rows[i].token,
              want[i]);

    unsigned long long max_g = 0;
    for (int i = 0; i < n; i++) {
        CHECK(rows[i].gseq > max_g, "gseq must strictly increase across the replayed timeline");
        max_g = rows[i].gseq;
    }

    /* 重启后续写：seq/gseq 必须从磁盘水位线继续，不能从 1 重新计数。 */
    CHECK(agent_task_ledger_progress("task-ledger-1", "plan-ledger-1", "resumed", NULL) == 0,
          "post-restart progress write failed");
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/progress", base);
    CHECK(count_events(dir) == 4, "post-restart progress count=%d want=4", count_events(dir));
    char name[256];
    CHECK(latest_event(dir, name, sizeof(name)) == 0 && hall_evt_seq(name) == 4,
          "post-restart seq must continue at 4");

    replay_row_t again[REPLAY_MAX_ROWS];
    int m = scan_events(base, again, REPLAY_MAX_ROWS);
    CHECK(m == 7, "post-restart replay saw %d events, want 7", m);
    qsort(again, (size_t)m, sizeof(again[0]), replay_row_cmp);
    CHECK(m > 0 && strcmp(again[m - 1].token, "progress:resumed") == 0,
          "replayed tail must be the post-restart event");
    CHECK(m > 0 && again[m - 1].gseq > max_g,
          "post-restart gseq must resume above the persisted watermark");
}


int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--replay-check") == 0) {
        const char *home = getenv("AIRY_HOME");
        if (!home || !*home) {
            printf("FAIL replay-check: AIRY_HOME missing\n");
            return 1;
        }
        replay_check(home);
        return g_failures ? 1 : 0;
    }

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

    /* 5b. 失败/取消同属终态：progress 必须是 1，不能回退到 0——
     *     看板按最新事件覆盖，回退会把已失败任务显示成未开始。 */
    CHECK(agent_task_ledger_result("task-ledger-fail", "plan-ledger-1", "failed") == 0,
          "failed result write failed");
    snprintf(dir, sizeof(dir), "%s/data/agentrt/hall/default/task-ledger-fail/result", home);
    body = read_event(dir);
    CHECK(body && strstr(body, "\"state\":\"failed\""), "failed state missing");
    CHECK(body && strstr(body, "\"progress\":1"), "failed terminal progress must be 1");
    free(body);

    /* 6. 空主键不产生无主目录。 */
    CHECK(agent_task_ledger_result(NULL, NULL, "failed") != 0, "empty key must be rejected");
    CHECK(agent_task_ledger_progress("", NULL, "executing", NULL) != 0,
          "empty key must be rejected");

    /* 7. 重启回放：exec 自身为新进程（内存态清零），只凭磁盘账本还原
     *    blueprint→progress×3→issue→result 全链，并续写第 7 条事件。 */
    pid_t pid = fork();
    if (pid == 0) {
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0)
            _exit(127);
        exe[n] = '\0';
        execl(exe, exe, "--replay-check", (char *)NULL);
        _exit(127);
    }
    CHECK(pid > 0, "fork for restart replay failed");
    if (pid > 0) {
        int st = 0;
        if (waitpid(pid, &st, 0) != pid)
            g_failures++;
        else
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "restart replay child failed (status=%d)", st);
    }

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
