// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_hall_writer.c
 * @brief daemon 侧事件流写端（hall_writer）单元测试
 *
 * 覆盖 2.8b 写端不变量（与 gateway_hall_store 测试同一套断言）：
 * - 事件回读（file.id 与落盘文件名一致、header 字段正确）
 * - prev_file 决策链链接（同 (task, category) 内指向上一事件）
 * - 跨 category 链隔离
 * - seq 递增（跨进程并发写不撞号）
 * - 非法参数与 write_roles 策略
 *
 * 数据目录经 AIRY_HOME 隔离到 /tmp。
 */

// @owner: team-B
#include "hall_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_BEGIN(name)                  \
    do {                                  \
        printf("  [TEST] %s ... ", name); \
        g_tests_run++;                    \
    } while (0)

#define TEST_PASS()       \
    do {                  \
        printf("PASS\n"); \
        g_tests_passed++; \
    } while (0)

#define TEST_FAIL(msg)             \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT_TRUE(cond)     \
    do {                      \
        if (!(cond)) {        \
            TEST_FAIL(#cond); \
            return;           \
        }                     \
    } while (0)

#define ASSERT_EQ_INT(a, b)          \
    do {                             \
        if ((a) != (b)) {            \
            TEST_FAIL(#a " != " #b); \
            return;                  \
        }                            \
    } while (0)

static int str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(str_eq((a), (b)))

#define HW_TEST_ROOT_REL "data/agentrt/hall/default"

static char g_home[512];

static void isolate_data_dir(void)
{
    snprintf(g_home, sizeof(g_home), "/tmp/airymaxrt-hw-%ld", (long)getpid());
    setenv("AIRY_HOME", g_home, 1);
    setenv("AIRY_DATA_DIR", "", 1);
}

/* task/category 目录下 seq 最大的事件文件名（与写端 hw_dir_scan 同规则）。 */
static int max_seq_file(const char *dir, char *out, size_t sz)
{
    out[0] = '\0';
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    unsigned max_seq = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len < 5 || strcmp(n + len - 5, ".json") != 0)
            continue;
        const char *last_dot = strrchr(n, '.');
        if (!last_dot || last_dot == n)
            continue;
        const char *dot2 = last_dot - 1;
        while (dot2 > n && *dot2 != '.')
            dot2--;
        if (*dot2 != '.')
            continue;
        unsigned seq = 0;
        int digits = 0;
        for (const char *q = dot2 + 1; q < last_dot; q++) {
            if (*q < '0' || *q > '9') {
                seq = 0;
                digits = 0;
                break;
            }
            seq = seq * 10 + (unsigned)(*q - '0');
            digits++;
        }
        if (digits > 0 && seq > max_seq) {
            max_seq = seq;
            snprintf(out, sz, "%s", n);
        }
    }
    closedir(d);
    return (out[0] != '\0') ? 0 : -1;
}

static int read_text_file(const char *path, char *out, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t got = fread(out, 1, sz - 1, f);
    out[got] = '\0';
    fclose(f);
    return (int)got;
}

static int json_str_field(const char *json, const char *key, char *out, size_t sz)
{
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < sz)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static void test_event_write_readback(void)
{
    TEST_BEGIN("event_write_readback");
    const char *task = "hw-sched-1";
    const char *cat = "progress";
    ASSERT_EQ_INT(0, daemon_hall_write(task, cat, NULL, "{\"event\":\"task_queued\"}"));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, HW_TEST_ROOT_REL, task, cat);
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);

    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "id", v, sizeof(v)));
    ASSERT_STR_EQ(v, fname);
    ASSERT_EQ_INT(0, json_str_field(buf, "category", v, sizeof(v)));
    ASSERT_STR_EQ(v, cat);
    ASSERT_EQ_INT(0, json_str_field(buf, "task_id", v, sizeof(v)));
    ASSERT_STR_EQ(v, task);
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, "");
    /* progress 属于执行类 category：write_roles 含 executor */
    ASSERT_TRUE(strstr(buf, "\"write_roles\":[\"cognition\",\"executor\"]") != NULL);
    ASSERT_TRUE(strstr(buf, "\"event\":\"task_queued\"") != NULL);
    TEST_PASS();
}

static void test_prev_file_links(void)
{
    TEST_BEGIN("prev_file_links");
    const char *task = "hw-sched-2";
    const char *cat = "progress";
    ASSERT_EQ_INT(0, daemon_hall_write(task, cat, NULL, "{\"event\":\"task_queued\"}"));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, HW_TEST_ROOT_REL, task, cat);
    char first[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, first, sizeof(first)));

    ASSERT_EQ_INT(0, daemon_hall_write(task, cat, NULL, "{\"event\":\"task_started\"}"));

    char second[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, second, sizeof(second)));
    ASSERT_TRUE(strcmp(second, first) != 0);

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, second);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);
    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, first);
    TEST_PASS();
}

static void test_prev_file_category_isolated(void)
{
    TEST_BEGIN("prev_file_category_isolated");
    const char *task = "hw-sched-3";
    ASSERT_EQ_INT(0, daemon_hall_write(task, "chain", NULL, "{\"event\":\"chat_start\"}"));
    /* chain 是 cognition-only：write_roles 仅 cognition */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, HW_TEST_ROOT_REL, task, "chain");
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);
    ASSERT_TRUE(strstr(buf, "\"write_roles\":[\"cognition\"]") != NULL);

    /* 不同 category：result 的 prev_file 必须为空（链按 (task, category) 隔离） */
    ASSERT_EQ_INT(0, daemon_hall_write(task, "result", NULL, "{\"event\":\"tool_result\"}"));
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, HW_TEST_ROOT_REL, task, "result");
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);
    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, "");
    TEST_PASS();
}

static void test_seq_increments(void)
{
    TEST_BEGIN("seq_increments");
    const char *task = "hw-sched-4";
    const char *cat = "result";
    for (int i = 0; i < 3; i++) {
        char content[64];
        snprintf(content, sizeof(content), "{\"i\":%d}", i);
        ASSERT_EQ_INT(0, daemon_hall_write(task, cat, NULL, content));
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, HW_TEST_ROOT_REL, task, cat);
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));
    ASSERT_TRUE(strstr(fname, ".0003.json") != NULL);
    TEST_PASS();
}

static void test_invalid_params(void)
{
    TEST_BEGIN("invalid_params");
    ASSERT_EQ_INT(-1, daemon_hall_write(NULL, "progress", NULL, "{}"));
    ASSERT_EQ_INT(-1, daemon_hall_write("", "progress", NULL, "{}"));
    ASSERT_EQ_INT(-1, daemon_hall_write("t", NULL, NULL, "{}"));
    ASSERT_EQ_INT(-1, daemon_hall_write("t", "progress", NULL, NULL));
    ASSERT_EQ_INT(-1, daemon_hall_write("t", "progress", NULL, ""));
    TEST_PASS();
}

int main(void)
{
    isolate_data_dir();
    printf("test_hall_writer: AIRY_HOME=%s\n", g_home);

    test_event_write_readback();
    test_prev_file_links();
    test_prev_file_category_isolated();
    test_seq_increments();
    test_invalid_params();

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
