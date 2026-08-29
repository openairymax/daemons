// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_cap_registry.c
 * @brief 统一能力网关 — 能力注册表单元测试（0.1.6 P1-4）
 *
 * 覆盖 SSoT 不变量：
 * - cap_key 唯一性（全表扫描，重复即失败）
 * - cap_key 可重构性（"<ns>.<method>" 语义一致性，防登记漂移）
 * - 关键能力存在性 + 处理方式（kind）断言（防误删特殊处理器）
 * - fail-closed：未登记能力 gw_cap_find 返回 NULL（仅暴露已声明能力）
 * - gw_cap_ns_timeout 已知/未知命名空间回退
 * - gw_cap_emit 事件埋点（AIRY_HOME 隔离到 /tmp，绝不触碰真实运行时目录）
 */

// @owner: team-B
#include "gateway_cap_registry.h"

#include "gateway_biz_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define ASSERT_EQ_INT(a, b)             \
    do {                                \
        if ((a) != (b)) {               \
            TEST_FAIL(#a " != " #b);    \
            return;                     \
        }                               \
    } while (0)

#define ASSERT_NOT_NULL(p)      \
    do {                        \
        if (!(p)) {             \
            TEST_FAIL(#p " is NULL"); \
            return;             \
        }                       \
    } while (0)

static void test_count_and_uniqueness(void)
{
    TEST_BEGIN("count + cap_key uniqueness/reconstruct");
    size_t n = gw_cap_count();
    ASSERT_TRUE(n > 100); /* 覆盖 16+ 命名空间全量能力 */
    for (size_t i = 0; i < n; ++i) {
        const gw_cap_t *ci = gw_cap_at(i);
        ASSERT_NOT_NULL(ci);
        ASSERT_TRUE(ci->cap_key && ci->ns && ci->method);
        size_t nsl = strlen(ci->ns);
        size_t ml = strlen(ci->method);
        /* cap_key 语义必须可重构为 "<ns>.<method>" */
        ASSERT_TRUE(strlen(ci->cap_key) == nsl + ml + 1);
        ASSERT_TRUE(strncmp(ci->cap_key, ci->ns, nsl) == 0);
        ASSERT_TRUE(ci->cap_key[nsl] == '.');
        ASSERT_TRUE(strcmp(ci->cap_key + nsl + 1, ci->method) == 0);
        /* cap_key 唯一性（SSoT：禁止重复登记） */
        for (size_t j = 0; j < i; ++j) {
            ASSERT_TRUE(strcmp(ci->cap_key, gw_cap_at(j)->cap_key) != 0);
        }
        /* 每个命名空间都应有转发超时（未知回退默认，恒 > 0） */
        ASSERT_TRUE(gw_cap_ns_timeout(ci->ns) > 0);
    }
    /* 越界访问返回 NULL */
    ASSERT_TRUE(gw_cap_at(n) == NULL);
    ASSERT_TRUE(gw_cap_at(n + 100) == NULL);
    TEST_PASS();
}

static void test_key_caps(void)
{
    TEST_BEGIN("key caps present with right kinds");
    const gw_cap_t *c;
    c = gw_cap_find("llm.complete");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    c = gw_cap_find("llm.list_models");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_LLM_LIST);
    c = gw_cap_find("agent.run");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_AGENT_RUN);
    c = gw_cap_find("agent.cancel");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_AGENT_RUN);
    c = gw_cap_find("tool.pending");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_TOOL_APPROVE);
    c = gw_cap_find("tool.approve");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_TOOL_APPROVE);
    c = gw_cap_find("mem.write");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_MEM);
    c = gw_cap_find("mem.kb_search");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_MEM);
    c = gw_cap_find("hall.board");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_HALL);
    c = gw_cap_find("hall.stream");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_HALL);
    c = gw_cap_find("think.process");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    c = gw_cap_find("sched.dag_submit");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    c = gw_cap_find("cupolas.vault_store");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    c = gw_cap_find("monit.heartbeat");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    c = gw_cap_find("a2a.send_message");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD);
    TEST_PASS();
}

static void test_fail_closed(void)
{
    TEST_BEGIN("fail-closed: unregistered caps rejected");
    ASSERT_TRUE(gw_cap_find("llm.complete") != NULL);
    ASSERT_TRUE(gw_cap_find("llm.foo") == NULL);
    ASSERT_TRUE(gw_cap_find("unknown.cap") == NULL);
    ASSERT_TRUE(gw_cap_find("agent.run.extra") == NULL);
    ASSERT_TRUE(gw_cap_find("agent") == NULL);
    ASSERT_TRUE(gw_cap_find("mem") == NULL);
    ASSERT_TRUE(gw_cap_find("") == NULL);
    ASSERT_TRUE(gw_cap_find(NULL) == NULL);
    TEST_PASS();
}

static void test_ns_timeout(void)
{
    TEST_BEGIN("ns timeout table");
    ASSERT_EQ_INT(gw_cap_ns_timeout("llm"), GW_LLM_DEFAULT_TIMEOUT_MS);
    ASSERT_EQ_INT(gw_cap_ns_timeout("think"), GW_THINK_TIMEOUT_MS);
    ASSERT_TRUE(gw_cap_ns_timeout("tool") > 0);
    ASSERT_TRUE(gw_cap_ns_timeout("sched") > 0);
    ASSERT_TRUE(gw_cap_ns_timeout("mem") > 0);
    /* 未知命名空间回退默认 */
    ASSERT_EQ_INT(gw_cap_ns_timeout("nope"), GW_TOOL_TIMEOUT_MS);
    ASSERT_EQ_INT(gw_cap_ns_timeout(NULL), GW_TOOL_TIMEOUT_MS);
    TEST_PASS();
}

static void test_emit_event(void)
{
    TEST_BEGIN("gw_cap_emit event recording");
    /* AIRY_HOME 隔离到 /tmp 唯一路径（airy_data_dir 首次调用时缓存，
     * 必须在任何事件写入前设置），绝不触碰真实运行时目录。 */
    char g_home[256];
    snprintf(g_home, sizeof(g_home), "/tmp/airymaxrt-gw-cap-%ld", (long)getpid());
    mkdir(g_home, 0700);
    setenv("AIRY_HOME", g_home, 1);
    setenv("AIRY_DATA_DIR", "", 1);
    /* 合法调用：不崩溃即通过（写端容错，事件记录失败不阻断能力调用） */
    gw_cap_emit("llm.complete", "ok", NULL);
    gw_cap_emit("llm.complete", "deny", "unregistered capability");
    gw_cap_emit("unknown.cap", "deny", NULL);
    /* 非法参数：直接忽略，不崩溃 */
    gw_cap_emit(NULL, "ok", NULL);
    gw_cap_emit("llm.complete", NULL, NULL);
    TEST_PASS();
}

int main(void)
{
    printf("test_gateway_cap_registry: AIRY_HOME isolated to /tmp\n");
    test_count_and_uniqueness();
    test_key_caps();
    test_fail_closed();
    test_ns_timeout();
    test_emit_event();
    printf("test_gateway_cap_registry: %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
