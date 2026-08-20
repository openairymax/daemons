// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_hall_store.c
 * @brief Gateway 事件流写端（gateway_hall_store）单元测试
 *
 * 覆盖 2.8a 修复点与 SSoT 写端不变量：
 * - prev_file 决策链链接（同 (task, category) 内指向前一个事件文件）
 * - 跨 category 隔离（不同 category 互不串链）
 * - 写后必读断言（debug 构建下事件文件可立即回读并含自身 file.id）
 * - node_id / write_roles / seq 递增 / 非法参数 / task_id 生成
 *
 * 数据目录通过 AIRY_HOME 隔离到 /tmp，绝不触碰真实运行时目录。
 */

// @owner: team-B
#include "gateway_hall_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

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

#define ASSERT_EQ_INT(a, b)  \
    do {                     \
        if ((a) != (b)) {    \
            TEST_FAIL(#a " != " #b); \
            return;          \
        }                    \
    } while (0)

/* 字符串相等辅助：函数形参为指针，避免宏内对数组/字面量取地址触发
 * -Waddress（数组与字符串字面量地址恒非空）。 */
static int str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(str_eq((a), (b)))

#define GW_HALL_TEST_ROOT_REL "data/agentrt/hall/default"

/* 隔离数据目录：AIRY_HOME 指向 /tmp 唯一路径（airy_data_dir 从
 * AIRY_HOME/data 解析，仅首次调用时缓存，故必须在任何事件写入前设置）。 */
static char g_home[512];

static void isolate_data_dir(void)
{
    snprintf(g_home, sizeof(g_home), "/tmp/airymaxrt-gw-hall-%ld", (long)getpid());
    setenv("AIRY_HOME", g_home, 1);
    setenv("AIRY_DATA_DIR", "", 1);
}

/* task/category 目录下 seq 最大的事件文件名（与写端 gw_hall_dir_scan 同规则）。 */
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
        /* seq 是倒数第二段（tenant.task.cat.ts.seq.json） */
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

/* 提取 JSON 字符串字段值（"key":"value"），header 字段均为字符串。 */
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
    const char *task = "gw-sse-1";
    const char *cat = "chain";
    ASSERT_EQ_INT(0, gw_hall_store_event(task, cat, "node-42", "{\"event\":\"chat_start\"}"));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, GW_HALL_TEST_ROOT_REL, task, cat);
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);

    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "id", v, sizeof(v)));
    ASSERT_STR_EQ(v, fname); /* file.id == 落盘文件名（单一真相源标识一致） */
    ASSERT_EQ_INT(0, json_str_field(buf, "category", v, sizeof(v)));
    ASSERT_STR_EQ(v, cat);
    ASSERT_EQ_INT(0, json_str_field(buf, "task_id", v, sizeof(v)));
    ASSERT_STR_EQ(v, task);
    ASSERT_EQ_INT(0, json_str_field(buf, "tenant_id", v, sizeof(v)));
    ASSERT_STR_EQ(v, "default");
    ASSERT_EQ_INT(0, json_str_field(buf, "node_id", v, sizeof(v)));
    ASSERT_STR_EQ(v, "node-42");
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, ""); /* 首事件 prev_file 为空 */
    ASSERT_TRUE(strstr(buf, "\"write_roles\":[\"cognition\"]") != NULL);
    ASSERT_TRUE(strstr(buf, "\"event\":\"chat_start\"") != NULL);
    TEST_PASS();
}

static void test_prev_file_links(void)
{
    TEST_BEGIN("prev_file_links");
    const char *task = "gw-sse-2";
    const char *cat = "chain";
    ASSERT_EQ_INT(0, gw_hall_store_event(task, cat, NULL, "{\"event\":\"chat_start\"}"));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, GW_HALL_TEST_ROOT_REL, task, cat);
    char first[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, first, sizeof(first)));

    ASSERT_EQ_INT(0, gw_hall_store_event(task, cat, NULL, "{\"event\":\"llm_round\"}"));

    char second[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, second, sizeof(second)));
    ASSERT_TRUE(strcmp(second, first) != 0); /* seq 递增，文件不同 */

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, second);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);

    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, first); /* 决策链 prev 链接指向上一事件文件 */
    TEST_PASS();
}

static void test_prev_file_category_isolated(void)
{
    TEST_BEGIN("prev_file_category_isolated");
    const char *task = "gw-sse-3";
    ASSERT_EQ_INT(0, gw_hall_store_event(task, "chain", NULL, "{\"event\":\"chat_start\"}"));
    /* 不同 category：progress 的 prev_file 必须为空（链按 (task, category) 隔离） */
    ASSERT_EQ_INT(0, gw_hall_store_event(task, "progress", NULL, "{\"status\":\"ok\"}"));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, GW_HALL_TEST_ROOT_REL, task, "progress");
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char buf[2048];
    ASSERT_TRUE(read_text_file(path, buf, sizeof(buf)) > 0);
    char v[256];
    ASSERT_EQ_INT(0, json_str_field(buf, "prev_file", v, sizeof(v)));
    ASSERT_STR_EQ(v, "");
    /* 执行类 category 的 write_roles 含 executor */
    ASSERT_TRUE(strstr(buf, "\"write_roles\":[\"cognition\",\"executor\"]") != NULL);
    TEST_PASS();
}

static void test_seq_increments(void)
{
    TEST_BEGIN("seq_increments");
    const char *task = "gw-sse-4";
    const char *cat = "result";
    for (int i = 0; i < 3; i++) {
        char content[64];
        snprintf(content, sizeof(content), "{\"i\":%d}", i);
        ASSERT_EQ_INT(0, gw_hall_store_event(task, cat, NULL, content));
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s/%s/%s", g_home, GW_HALL_TEST_ROOT_REL, task, cat);
    char fname[512];
    ASSERT_EQ_INT(0, max_seq_file(dir, fname, sizeof(fname)));
    ASSERT_TRUE(strstr(fname, ".0003.json") != NULL); /* 3 个事件 → max seq=3 */
    TEST_PASS();
}

static void test_invalid_params(void)
{
    TEST_BEGIN("invalid_params");
    ASSERT_EQ_INT(-1, gw_hall_store_event(NULL, "chain", NULL, "{}"));
    ASSERT_EQ_INT(-1, gw_hall_store_event("", "chain", NULL, "{}"));
    ASSERT_EQ_INT(-1, gw_hall_store_event("t", NULL, NULL, "{}"));
    ASSERT_EQ_INT(-1, gw_hall_store_event("t", "chain", NULL, NULL));
    ASSERT_EQ_INT(-1, gw_hall_store_event("t", "chain", NULL, ""));
    TEST_PASS();
}

static void test_task_id_now(void)
{
    TEST_BEGIN("task_id_now");
    char out[64] = {0};
    gw_hall_task_id_now(out, sizeof(out));
    ASSERT_TRUE(strncmp(out, "gw-", 3) == 0);
    /* "gw-" + 4-2-2 T 2-2-2-3 毫秒 = 18 位时间戳 */
    ASSERT_EQ_INT(21, (int)strlen(out));
    char small[8] = {0};
    gw_hall_task_id_now(small, sizeof(small)); /* 小缓冲不越界 */
    ASSERT_EQ_INT(0, small[sizeof(small) - 1]);
    TEST_PASS();
}

int main(void)
{
    isolate_data_dir();
    printf("test_gateway_hall_store: AIRY_HOME=%s\n", g_home);

    test_event_write_readback();
    test_prev_file_links();
    test_prev_file_category_isolated();
    test_seq_increments();
    test_invalid_params();
    test_task_id_now();

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
