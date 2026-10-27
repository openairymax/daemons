// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file test_svc_model_watch.c
 * @brief model.yaml 漂移检测与热应用入口（svc_model_watch）单元测试
 *
 * 覆盖 0.1.19 §3.5 缺陷 5 与 0.1.20 D2-7 的判定契约：
 * - 参数守卫 fail-closed，快照缺席时检测静默停用而非误报漂移
 * - 元数据未变走廉价路径返回「无漂移」
 * - 内容变化 → 漂移（一次性 WARN + 运行时目录落 stale 标记）
 * - touch（mtime 变、内容同）不误报，且刷新元数据基线
 * - 配置文件删除亦属漂移
 * - 重新 init 清除本 daemon 陈旧标记
 * - tick 按周期节流，到期才检测；首次漂移发 config_drift（applied=false）
 * - reload 成功：世代 +1、重照基线、发 config_applied
 * - reload 失败：保留旧基线、发 config_reload_failed 且带原因
 *
 * 数据目录经 AIRY_HOME 隔离到 /tmp，避免污染真实运行时目录；事件断言
 * 直读 hall 落盘目录（SSoT 路径由 hall_event.h 常量给出）。
 */

#include "svc_model_watch.h"

#include "hall_event.h"
#include "platform_paths.h"

#include <dirent.h>
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

/* 事件落盘目录：{data}/agentrt/hall/default/{task}/issue。
 * 路径分量全部取自 hall_event.h 常量，避免测试复刻布局。 */
static void evt_dir_of(const char *task, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s/%s/default/%s/issue", airy_data_dir(), HALL_EVT_ROOT_REL, task);
}

static int evt_count(const char *task)
{
    char dir[900];
    evt_dir_of(task, dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".json") == 0)
            n++;
    }
    closedir(d);
    return n;
}

/* 取字典序最大的事件文件（ts 定宽 + seq 定宽，故字典序即写入序） */
static int evt_latest(const char *task, char *out, size_t cap)
{
    char dir[900];
    evt_dir_of(task, dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d)
        return -1;

    char name[256] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".json") == 0 &&
            strcmp(ent->d_name, name) > 0)
            snprintf(name, sizeof(name), "%s", ent->d_name);
    }
    closedir(d);
    if (!name[0])
        return -1;

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

/* apply 回调替身：只模拟应用结论，不做任何副作用 */
static int g_apply_rc = 0;
static int g_apply_calls = 0;

static int fake_apply(void *ud, char *why, size_t cap)
{
    (void)ud;
    g_apply_calls++;
    if (g_apply_rc < 0 && why && cap > 0)
        snprintf(why, cap, "yaml parse error at line 3");
    return g_apply_rc;
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

static void test_tick_interval(void)
{
    svc_mw_t w;

    TEST_BEGIN("tick_interval");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "llm_d"), 0);

    /* 首轮立即检测（last_ms 为 0，不参与节流） */
    ASSERT_EQ_INT(svc_mw_tick(&w, 1000), 0);

    write_cfg("version: 2-longer-payload\n");
    /* 未到周期：哪怕内容已变也不检测，避免每 tick 一次整读 */
    ASSERT_EQ_INT(svc_mw_tick(&w, 2000), 0);
    /* 到期：检出漂移 */
    ASSERT_EQ_INT(svc_mw_tick(&w, 6000), 1);
    TEST_PASS();
}

static void test_drift_event(void)
{
    svc_mw_t w;
    char json[4096];

    TEST_BEGIN("drift_event");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "llm_d"), 0);
    ASSERT_EQ_INT(svc_mw_tick(&w, 1000), 0);

    int before = evt_count("llm_d");
    /* 变长写入：tmpfs 时间戳粒度粗，同尺寸改写可能落在同一 mtime 刻度内，
     * 会被廉价路径视为未变——此处刻意避开该边界，专测事件契约 */
    write_cfg("version: 2-with-different-length\n");
    ASSERT_EQ_INT(svc_mw_tick(&w, 7000), 1);
    ASSERT_EQ_INT(evt_count("llm_d"), before + 1);

    ASSERT_EQ_INT(evt_latest("llm_d", json, sizeof(json)), 0);
    ASSERT_TRUE(strstr(json, "config_drift") != NULL);
    ASSERT_TRUE(strstr(json, "\"applied\":false") != NULL);

    /* 同一漂移只报一次事件，但检测结论仍如实返回 */
    ASSERT_EQ_INT(svc_mw_tick(&w, 14000), 1);
    ASSERT_EQ_INT(evt_count("llm_d"), before + 1);
    TEST_PASS();
}

static void test_reload_ok(void)
{
    svc_mw_t w;
    char json[4096];

    TEST_BEGIN("reload_ok");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "llm_d"), 0);
    ASSERT_EQ_INT(svc_mw_hook(&w, fake_apply, NULL, 0), 0);

    /* 变长写入：同 test_drift_event，避开 tmpfs mtime 粗粒度导致的
     * 「同尺寸改写落在同一刻度」边界，专测 reload 成功路径 */
    write_cfg("version: 2-longer-payload\n");
    ASSERT_EQ_INT(svc_mw_check(&w), 1);

    int before = evt_count("llm_d");
    g_apply_rc = 0;
    g_apply_calls = 0;
    ASSERT_EQ_INT(svc_mw_reload(&w), 0);
    ASSERT_EQ_INT(g_apply_calls, 1);
    ASSERT_EQ_INT((int)w.generation, 1);

    /* 应用成功即重照基线：同一文件不再报漂移 */
    ASSERT_EQ_INT(svc_mw_check(&w), 0);
    ASSERT_TRUE(!marker_exists());

    ASSERT_EQ_INT(evt_count("llm_d"), before + 1);
    ASSERT_EQ_INT(evt_latest("llm_d", json, sizeof(json)), 0);
    ASSERT_TRUE(strstr(json, "config_applied") != NULL);
    ASSERT_TRUE(strstr(json, "\"generation\":1") != NULL);
    TEST_PASS();
}

static void test_reload_fail(void)
{
    svc_mw_t w;
    char json[4096];

    TEST_BEGIN("reload_fail");
    write_cfg("version: 1\n");
    ASSERT_EQ_INT(svc_mw_init(&w, g_cfg, "llm_d"), 0);
    ASSERT_EQ_INT(svc_mw_hook(&w, fake_apply, NULL, 0), 0);

    write_cfg("version: broken\n");

    int before = evt_count("llm_d");

    /* 忙/退避：既不算失败也不产生事件，等下一次显式请求 */
    g_apply_rc = 1;
    g_apply_calls = 0;
    ASSERT_EQ_INT(svc_mw_reload(&w), 1);
    ASSERT_EQ_INT(g_apply_calls, 1);
    ASSERT_EQ_INT(evt_count("llm_d"), before);
    ASSERT_EQ_INT((int)w.generation, 0);

    /* 真失败：发 config_reload_failed 且带原因，旧基线必须保留 */
    g_apply_rc = -1;
    ASSERT_EQ_INT(svc_mw_reload(&w), -1);
    ASSERT_EQ_INT((int)w.generation, 0);
    ASSERT_EQ_INT(svc_mw_check(&w), 1);

    ASSERT_EQ_INT(evt_count("llm_d"), before + 1);
    ASSERT_EQ_INT(evt_latest("llm_d", json, sizeof(json)), 0);
    ASSERT_TRUE(strstr(json, "config_reload_failed") != NULL);
    ASSERT_TRUE(strstr(json, "yaml parse error") != NULL);
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
    test_tick_interval();
    test_drift_event();
    test_reload_ok();
    test_reload_fail();

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", g_home);
    if (system(cleanup) != 0) {
        printf("  [WARN] cleanup failed: %s\n", g_home);
    }

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
