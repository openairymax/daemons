// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file test_svc_model_watch.c
 * @brief model.yaml 启动快照 / 请求侧漂移检测（svc_model_watch）单元测试
 *
 * 覆盖 0.1.19 §3.5 缺陷 5 的判定契约：
 * - 参数守卫 fail-closed，快照缺席时检测静默停用而非误报漂移
 * - 元数据未变走廉价路径返回「无漂移」
 * - 内容变化 → 漂移（一次性 WARN + 运行时目录落 stale 标记）
 * - touch（mtime 变、内容同）不误报，且刷新元数据基线
 * - 配置文件删除亦属漂移
 * - 重新 init 清除本 daemon 陈旧标记
 *
 * 数据目录经 AIRY_HOME 隔离到 /tmp，避免污染真实运行时目录。
 */

#include "svc_model_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

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

static char g_home[512];
static char g_cfg[600];
static char g_marker[600];

static void isolate_data_dir(void)
{
    snprintf(g_home, sizeof(g_home), "/tmp/airymaxrt-mw-%ld", (long)getpid());
    snprintf(g_cfg, sizeof(g_cfg), "%s/model.yaml", g_home);
    snprintf(g_marker, sizeof(g_marker), "%s/run/model_yaml_stale_think_d", g_home);
    mkdir(g_home, 0700);
    char run_dir[600];
    snprintf(run_dir, sizeof(run_dir), "%s/run", g_home);
    mkdir(run_dir, 0700);
    setenv("AIRY_HOME", g_home, 1);
}

static void write_cfg(const char *text)
{
    FILE *f = fopen(g_cfg, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int marker_exists(void)
{
    struct stat st;
    return stat(g_marker, &st) == 0;
}

static void set_mtime(int64_t sec)
{
    struct utimbuf tb;
    tb.actime = (time_t)sec;
    tb.modtime = (time_t)sec;
    utime(g_cfg, &tb);
}

static void test_init_guards(void)
{
    svc_mw_t w;

    TEST_BEGIN("init_guards");
    ASSERT_EQ_INT(svc_mw_init(NULL, g_cfg, "think_d"), -1);
    ASSERT_EQ_INT(svc_mw_init(&w, NULL, "think_d"), -1);
    ASSERT_EQ_INT(svc_mw_init(&w, "", "think_d"), -1);
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, NULL), -1);
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, ""), -1);
    TEST_PASS();
}

static void test_snapshot_absent(void)
{
    svc_mw_t w;
    char absent[600];

    TEST_BEGIN("snapshot_absent");
    snprintf(absent, sizeof(absent), "%s/missing.yaml", g_home);
    ASSERT_EQ_INT(svc_mw_init(&w, absent, "llm_d"), -1);
    ASSERT_EQ_INT(w.valid, 0);
    ASSERT_EQ_INT(svc_mw_check(&w), -1);
    TEST_PASS();
}

static void test_snapshot_no_drift(void)
{
    svc_mw_t w;

    TEST_BEGIN("snapshot_no_drift");
    write_cfg("version: 1\nthink:\n  model: m1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);
    ASSERT_EQ_INT(w.valid, 1);
    if (svc_mw_check(&w) != 0) {
        TEST_FAIL("check != 0 on unchanged file");
        return;
    }
    ASSERT_TRUE(!marker_exists());
    TEST_PASS();
}

static void test_content_change_detects(void)
{
    svc_mw_t w;

    TEST_BEGIN("content_change_detects");
    write_cfg("version: 1\nthink:\n  model: m1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);
    ASSERT_EQ_INT(svc_mw_check(&w), 0);

    write_cfg("version: 1\nthink:\n  model: m2-with-longer-name\n");
    ASSERT_EQ_INT(svc_mw_check(&w), 1);
    ASSERT_TRUE(marker_exists());
    /* 已告警后再次请求仍如实返回漂移（不因 warn 一次性而吞信号） */
    ASSERT_EQ_INT(svc_mw_check(&w), 1);
    TEST_PASS();
}

static void test_touch_no_false_alarm(void)
{
    svc_mw_t w;

    TEST_BEGIN("touch_no_false_alarm");
    write_cfg("version: 1\nthink:\n  model: m1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);

    set_mtime(1000000000);
    write_cfg("version: 1\nthink:\n  model: m1\n");
    set_mtime(1000000100);
    ASSERT_EQ_INT(svc_mw_check(&w), 0);
    ASSERT_TRUE(!marker_exists());
    /* 基线已刷新：再次触摸亦不误报 */
    set_mtime(1000000200);
    ASSERT_EQ_INT(svc_mw_check(&w), 0);
    TEST_PASS();
}

static void test_deleted_is_drift(void)
{
    svc_mw_t w;

    TEST_BEGIN("deleted_is_drift");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);
    remove(g_cfg);
    ASSERT_EQ_INT(svc_mw_check(&w), 1);
    ASSERT_TRUE(marker_exists());
    TEST_PASS();
}

static void test_init_clears_marker(void)
{
    svc_mw_t w;

    TEST_BEGIN("init_clears_marker");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);
    write_cfg("version: 22\n");
    ASSERT_EQ_INT(svc_mw_check(&w), 1);
    ASSERT_TRUE(marker_exists());

    /* 重启：新快照在位即清陈旧信号 */
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "think_d"), 0);
    ASSERT_TRUE(!marker_exists());
    ASSERT_EQ_INT(svc_mw_check(&w), 0);
    TEST_PASS();
}

int main(void)
{
    printf("svc_model_watch unit tests\n");
    isolate_data_dir();

    test_init_guards();
    test_snapshot_absent();
    test_snapshot_no_drift();
    test_content_change_detects();
    test_touch_no_false_alarm();
    test_deleted_is_drift();
    test_init_clears_marker();

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", g_home);
    if (system(cleanup) != 0) {
        printf("  [WARN] cleanup failed: %s\n", g_home);
    }

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
