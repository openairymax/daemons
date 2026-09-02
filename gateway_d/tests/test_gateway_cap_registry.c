// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_cap_registry.c
 * @brief 统一能力网关 — 能力注册表单元测试（0.1.6 P1-4）
 *
 * 覆盖 SSoT 不变量：
 * - cap_key 唯一性（全表扫描，重复即失败）
 * - cap_key 可重构性（"<ns>.<method>" 语义一致性，防登记漂移；
 *   legacy 整编条目——路由 ns 与 cap_key 首段不一致——按路由规则断言）
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

/* cap_key 首段（外部旧命名空间）!= 路由目标 ns → legacy 整编条目
 * （0.1.9 M4：plugin.* → tool、info.* / observe.* → monit）。 */
static int cap_external_ns(const gw_cap_t *ci)
{
    const char *dot = strchr(ci->cap_key, '.');
    if (!dot)
        return 0;
    size_t l = (size_t)(dot - ci->cap_key);
    return !(strlen(ci->ns) == l && strncmp(ci->cap_key, ci->ns, l) == 0);
}

/* legacy 整编条目路由断言：wire 方法为 "<旧ns>_<cap_key 尾段>" 前缀变体，
 * 或宿主未登记前缀变体时透传的 L2 标准方法（与 cap_key 尾段同名）。 */
static void cap_legacy_route(const gw_cap_t *ci)
{
    const char *dot = strchr(ci->cap_key, '.');
    ASSERT_NOT_NULL(dot);
    char prefix[64];
    int pn = snprintf(prefix, sizeof(prefix), "%.*s_", (int)(dot - ci->cap_key), ci->cap_key);
    ASSERT_TRUE(pn > 0 && (size_t)pn < sizeof(prefix));
    size_t pl = (size_t)pn;
    if (strncmp(ci->method, prefix, pl) == 0) {
        ASSERT_TRUE(strcmp(ci->method + pl, dot + 1) == 0);
    } else {
        ASSERT_TRUE(strcmp(ci->method, dot + 1) == 0);
        ASSERT_TRUE(strcmp(ci->method, "shutdown") == 0 ||
                    strcmp(ci->method, "get_stats") == 0 ||
                    strcmp(ci->method, "health_check") == 0);
    }
}

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
        /* cap_key 语义：默认 "<ns>.<method>"（SSoT 不变量）。
         * legacy 整编条目（0.1.9 M4：plugin.* → tool、info.* / observe.* →
         * monit）cap_key 保持外部契约不变、路由目标与 wire 方法名单独
         * 登记，不满足重构等式，按 cap_legacy_route 断言路由语义。 */
        if (cap_external_ns(ci)) {
            cap_legacy_route(ci);
        } else {
            ASSERT_TRUE(strlen(ci->cap_key) == nsl + ml + 1);
            ASSERT_TRUE(strncmp(ci->cap_key, ci->ns, nsl) == 0);
            ASSERT_TRUE(ci->cap_key[nsl] == '.');
            ASSERT_TRUE(strcmp(ci->cap_key + nsl + 1, ci->method) == 0);
        }
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
    /* M2-S3：policy.* 统一 RPC 面（外部 namespace policy → cupolas_d） */
    c = gw_cap_find("policy.load");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD && strcmp(c->ns, "cupolas") == 0);
    c = gw_cap_find("policy.activate");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD && strcmp(c->ns, "cupolas") == 0);
    c = gw_cap_find("policy.rollback");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD && strcmp(c->ns, "cupolas") == 0);
    c = gw_cap_find("policy.status");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_FWD && strcmp(c->ns, "cupolas") == 0);
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

static void test_contract_version(void)
{
    TEST_BEGIN("contract version (P1-4 SSoT)");
    /* 显式登记版本的能力 */
    ASSERT_EQ_INT(gw_cap_version("llm.complete"), 1);
    ASSERT_EQ_INT(gw_cap_version("agent.run"), 1);
    /* 未登记版本的能力 = 默认契约版本 1 */
    ASSERT_EQ_INT(gw_cap_version("llm.health_check"), 1);
    ASSERT_EQ_INT(gw_cap_version("mem.recent"), 1);
    /* 未登记能力 = -1 */
    ASSERT_EQ_INT(gw_cap_version("unknown.cap"), -1);
    ASSERT_EQ_INT(gw_cap_version(NULL), -1);
    /* 版本比对：匹配通过 / 不匹配拒绝 / 未声明通过 / 未登记 -1 */
    ASSERT_EQ_INT(gw_cap_check_version("llm.complete", 1), 0);
    ASSERT_EQ_INT(gw_cap_check_version("llm.complete", 2), 1);
    ASSERT_EQ_INT(gw_cap_check_version("llm.complete", 0), 0);
    ASSERT_EQ_INT(gw_cap_check_version("llm.complete", -1), 0);
    ASSERT_EQ_INT(gw_cap_check_version("unknown.cap", 1), -1);
    ASSERT_EQ_INT(gw_cap_check_version(NULL, 1), -1);
    TEST_PASS();
}

static void test_perm_requirements(void)
{
    TEST_BEGIN("perm requirements (P1-4 SSoT)");
    /* 高敏能力声明所需权限 */
    ASSERT_TRUE(strcmp(gw_cap_perm_for("agent.spawn"), "cap:agent.control") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("plugin.load"), "cap:plugin.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("mem.delete"), "cap:mem.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("cupolas.add_rule"), "cap:cupolas.admin") == 0);
    /* M2-S3：策略演化写操作管理员级，读操作默认放行 */
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.load"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.activate"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.rollback"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(gw_cap_perm_for("policy.status") == NULL);
    /* 日常核心链路能力：无额外权限要求（默认放行） */
    ASSERT_TRUE(gw_cap_perm_for("llm.complete") == NULL);
    ASSERT_TRUE(gw_cap_perm_for("think.process") == NULL);
    ASSERT_TRUE(gw_cap_perm_for("agent.run") == NULL);
    ASSERT_TRUE(gw_cap_perm_for("mem.write") == NULL);
    /* 未登记能力：NULL */
    ASSERT_TRUE(gw_cap_perm_for("unknown.cap") == NULL);
    ASSERT_TRUE(gw_cap_perm_for(NULL) == NULL);
    TEST_PASS();
}

static void test_ns_ownership(void)
{
    TEST_BEGIN("namespace exclusive ownership (0.1.9 S5)");
    /* 全表一致性：命名空间均有归属、FWD 转发目标与归属一致 → 0 */
    ASSERT_EQ_INT(gw_cap_ns_validate(), 0);
    /* M4 整编归属：旧命名空间归宿主 daemon 独占 */
    const gw_cap_t *c = gw_cap_find("plugin.load");
    ASSERT_TRUE(c && strcmp(c->ns, "tool") == 0);
    c = gw_cap_find("info.system");
    ASSERT_TRUE(c && strcmp(c->ns, "monit") == 0);
    c = gw_cap_find("observe.record_metric");
    ASSERT_TRUE(c && strcmp(c->ns, "monit") == 0);
    /* 特殊处理项（网关内）不受 FWD 归属检查约束但须已登记 */
    c = gw_cap_find("hall.board");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_HALL);
    c = gw_cap_find("mem.write");
    ASSERT_TRUE(c && c->kind == GW_CAP_KIND_MEM && strcmp(c->ns, "mem") == 0);
    TEST_PASS();
}

int main(void)
{
    printf("test_gateway_cap_registry: AIRY_HOME isolated to /tmp\n");
    test_count_and_uniqueness();
    test_key_caps();
    test_fail_closed();
    test_ns_timeout();
    test_contract_version();
    test_perm_requirements();
    test_ns_ownership();
    test_emit_event();
    printf("test_gateway_cap_registry: %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
