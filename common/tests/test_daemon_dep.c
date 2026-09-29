// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file test_daemon_dep.c
 * @brief daemon 硬依赖声明/启动期探测/降级态上报（daemon_dep）单元测试
 *
 * 覆盖 0.1.19 §7.2 运行不变量 2/3：
 * - 参数与容量契约（fail-closed，无一静默放行）
 * - 无依赖 ready、optional 缺失不降级
 * - required 缺失 → missing/ready/note/report 语义
 * - sdh==NULL 时全部依赖不可达（fail-closed），但探测本身不算错误
 * - 经真实 SD helper 的注册/发现闭环验证「可达 = SD 中登记且 healthy」
 *
 * 数据目录经 AIRY_HOME 隔离到 /tmp，避免污染真实 hall 事件库。
 */

#include "daemon_dep.h"
#include "service_discovery_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    return a && b && strcmp(a, b) == 0;
}

#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(str_eq((a), (b)))

static char g_home[512];

static void isolate_data_dir(void)
{
    snprintf(g_home, sizeof(g_home), "/tmp/airymaxrt-dep-%ld", (long)getpid());
    setenv("AIRY_HOME", g_home, 1);
    setenv("AIRY_DATA_DIR", "", 1);
}

static void test_init_guards(void)
{
    daemon_dep_t dep;
    daemon_dep_spec_t spec = {"svc", true};
    daemon_dep_spec_t too_many[DAEMON_DEP_MAX + 1];

    TEST_BEGIN("init_guards");
    ASSERT_EQ_INT(AIRY_ERR_INVALID_PARAM, daemon_dep_init(NULL, NULL, 0));
    ASSERT_EQ_INT(AIRY_ERR_INVALID_PARAM, daemon_dep_init(&dep, NULL, 1));
    /* count==0 时不要求 specs 非空 */
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_init(&dep, &spec, 0));
    memset(too_many, 0, sizeof(too_many));
    ASSERT_EQ_INT(AIRY_ERR_BUFFER_TOO_SMALL,
                  daemon_dep_init(&dep, too_many, DAEMON_DEP_MAX + 1));
    ASSERT_EQ_INT(AIRY_ERR_INVALID_PARAM, daemon_dep_probe(NULL, NULL));
    ASSERT_EQ_INT(-1, daemon_dep_note(&dep, NULL, 0));
    ASSERT_EQ_INT(-1, daemon_dep_note(&dep, g_home, 0));
    ASSERT_TRUE(!daemon_dep_ready(NULL));
    ASSERT_EQ_INT(0, (int)daemon_dep_missing(NULL));
    ASSERT_EQ_INT(0, (int)daemon_dep_count(NULL));
    ASSERT_EQ_INT(AIRY_ERR_INVALID_PARAM, daemon_dep_report(NULL, "x"));
    TEST_PASS();
}

static void test_empty_ready(void)
{
    daemon_dep_t dep;

    TEST_BEGIN("empty_ready");
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_init(&dep, NULL, 0));
    ASSERT_EQ_INT(0, (int)daemon_dep_count(&dep));
    /* 未探测：即便没有依赖也不得就绪（probed 门控） */
    ASSERT_TRUE(!daemon_dep_ready(&dep));
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_probe(&dep, NULL));
    ASSERT_TRUE(daemon_dep_ready(&dep));
    ASSERT_EQ_INT(0, (int)daemon_dep_missing(&dep));
    TEST_PASS();
}

static void test_optional_not_degrading(void)
{
    daemon_dep_t dep;
    static const daemon_dep_spec_t specs[] = {{"dep_absent_opt", false}};
    char note[256];

    TEST_BEGIN("optional_not_degrading");
    ASSERT_EQ_INT(AIRY_SUCCESS,
                  daemon_dep_init(&dep, specs, sizeof(specs) / sizeof(specs[0])));
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_probe(&dep, NULL));
    ASSERT_TRUE(daemon_dep_ready(&dep));
    ASSERT_EQ_INT(0, (int)daemon_dep_missing(&dep));
    ASSERT_EQ_INT(0, daemon_dep_note(&dep, note, sizeof(note)));
    ASSERT_STR_EQ(note, "");
    ASSERT_EQ_INT(0, daemon_dep_report(&dep, "test_dep_d"));
    TEST_PASS();
}

static void test_required_missing_fail_closed(void)
{
    daemon_dep_t dep;
    static const daemon_dep_spec_t specs[] = {
        {"dep_absent_req", true},
        {"dep_absent_opt", false},
    };
    char note[256];
    const char *name = NULL;
    bool required = false;
    bool reachable = true;

    TEST_BEGIN("required_missing_fail_closed");
    ASSERT_EQ_INT(AIRY_SUCCESS,
                  daemon_dep_init(&dep, specs, sizeof(specs) / sizeof(specs[0])));
    /* sdh 为 NULL：全部不可达，但探测是完成的（不是错误） */
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_probe(&dep, NULL));
    ASSERT_TRUE(!daemon_dep_ready(&dep));
    ASSERT_EQ_INT(1, (int)daemon_dep_missing(&dep));
    ASSERT_EQ_INT(2, (int)daemon_dep_count(&dep));

    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_at(&dep, 0, &name, &required, &reachable));
    ASSERT_STR_EQ(name, "dep_absent_req");
    ASSERT_TRUE(required);
    ASSERT_TRUE(!reachable);

    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_at(&dep, 1, &name, &required, &reachable));
    ASSERT_STR_EQ(name, "dep_absent_opt");
    ASSERT_TRUE(!required);
    ASSERT_TRUE(!reachable);
    ASSERT_EQ_INT(AIRY_ERR_INVALID_PARAM, daemon_dep_at(&dep, 2, NULL, NULL, NULL));

    ASSERT_EQ_INT((int)strlen("dep_absent_req"),
                  daemon_dep_note(&dep, note, sizeof(note)));
    ASSERT_STR_EQ(note, "dep_absent_req");
    /* report 返回缺失条数，并落一条 hall issue 事件 */
    ASSERT_EQ_INT(1, daemon_dep_report(&dep, "test_dep_d"));
    TEST_PASS();
}

static void test_note_truncates(void)
{
    daemon_dep_t dep;
    static const daemon_dep_spec_t specs[] = {
        {"dep_absent_aaaa", true},
        {"dep_absent_bbbb", true},
    };
    char note[8];

    TEST_BEGIN("note_truncates");
    ASSERT_EQ_INT(AIRY_SUCCESS,
                  daemon_dep_init(&dep, specs, sizeof(specs) / sizeof(specs[0])));
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_probe(&dep, NULL));
    ASSERT_EQ_INT(2, (int)daemon_dep_missing(&dep));
    /* 容量不足以放下首项 + NUL 时安全返回 0，不越界 */
    ASSERT_EQ_INT(0, daemon_dep_note(&dep, note, sizeof(note)));
    ASSERT_STR_EQ(note, "");
    TEST_PASS();
}

static void test_real_sd_reachable(void)
{
    daemon_dep_t dep;
    static const daemon_dep_spec_t specs[] = {
        {"dep_live_svc_x", true},
        {"dep_dead_svc_x", true},
    };
    const char *name = NULL;
    bool required = false;
    bool reachable = false;
    sd_helper_t *sdh;

    TEST_BEGIN("real_sd_reachable");
    sdh = sd_helper_init(NULL);
    ASSERT_TRUE(sdh != NULL);
    ASSERT_EQ_INT(0, sd_helper_register(sdh, "dep_live_svc_x", "test", "127.0.0.1", 18080,
                                        NULL, 30000));

    ASSERT_EQ_INT(AIRY_SUCCESS,
                  daemon_dep_init(&dep, specs, sizeof(specs) / sizeof(specs[0])));
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_probe(&dep, sdh));

    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_at(&dep, 0, &name, &required, &reachable));
    ASSERT_TRUE(reachable);
    ASSERT_EQ_INT(AIRY_SUCCESS, daemon_dep_at(&dep, 1, &name, &required, &reachable));
    ASSERT_TRUE(!reachable);
    ASSERT_TRUE(!daemon_dep_ready(&dep));
    ASSERT_EQ_INT(1, (int)daemon_dep_missing(&dep));
    ASSERT_EQ_INT(1, daemon_dep_report(&dep, "test_dep_d"));

    sd_helper_shutdown(sdh);
    TEST_PASS();
}

int main(void)
{
    isolate_data_dir();
    printf("test_daemon_dep: AIRY_HOME=%s\n", g_home);

    test_init_guards();
    test_empty_ready();
    test_optional_not_degrading();
    test_required_missing_fail_closed();
    test_note_truncates();
    test_real_sd_reachable();

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
