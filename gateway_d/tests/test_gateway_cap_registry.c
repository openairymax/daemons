// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gateway_cap_registry.c
 * @brief 统一能力网关 — 能力注册表单元测试
 *
 * 覆盖 SSoT 不变量：
 * - cap_key 唯一性（全表扫描，重复即失败）
 * - cap_key 可重构性（"<ns>.<method>" 语义一致性，防登记漂移；
 *   legacy 整编条目——路由 ns 与 cap_key 首段不一致——按路由规则断言）
 * - 关键能力存在性 + 处理方式（kind）断言（防误删特殊处理器）
 * - fail-closed：未登记能力 gw_cap_find 返回 NULL（仅暴露已声明能力）
 * - 命名空间策略表（gateway/.manifest 构建期生成）：归属/端点/前缀/可路由、
 *   尾点容忍、未知 ns fail-closed、槽位边界、l2 透传方法集
 * - gw_cap_ns_timeout 已知/未知命名空间回退
 * - gw_cap_emit 事件埋点（AIRY_HOME 隔离到 /tmp，绝不触碰真实运行时目录）
 */

// @owner: team-B
#include "gateway_cap_registry.h"

#include "gateway_biz_internal.h"
#include "gateway_policy.h"

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

/* 关键能力期望表：cap_key → kind 与归属命名空间（ns 为 NULL 表不校验归属）。
 * 期望值集中声明，增删能力只改一行，避免逐条 if 断言散落在流程里。 */
static const struct {
    const char *cap;
    gw_cap_kind_t kind;
    const char *ns;
} k_key_caps[] = {
    {"llm.complete", GW_CAP_KIND_FWD, NULL},
    {"llm.list_models", GW_CAP_KIND_LLM_LIST, NULL},
    {"agent.run", GW_CAP_KIND_AGENT_RUN, NULL},
    {"agent.cancel", GW_CAP_KIND_AGENT_RUN, NULL},
    {"tool.pending", GW_CAP_KIND_TOOL_APPROVE, NULL},
    {"tool.approve", GW_CAP_KIND_TOOL_APPROVE, NULL},
    {"mem.write", GW_CAP_KIND_MEM, NULL},
    {"mem.kb_search", GW_CAP_KIND_MEM, NULL},
    /* 语义缓存 / 上下文台账 / 提示词压缩三类能力必须可在网关层解析；
     * 缺失即 fail-closed（-32601），主链路无处接入。 */
    {"mem.cache_put", GW_CAP_KIND_MEM, NULL},
    {"mem.cache_get", GW_CAP_KIND_MEM, NULL},
    {"mem.cache_del", GW_CAP_KIND_MEM, NULL},
    {"mem.cache_stats", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_append", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_window", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_budget", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_mark", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_history", GW_CAP_KIND_MEM, NULL},
    {"mem.ledger_stats", GW_CAP_KIND_MEM, NULL},
    {"mem.compress", GW_CAP_KIND_MEM, NULL},
    {"hall.board", GW_CAP_KIND_HALL, NULL},
    {"hall.stream", GW_CAP_KIND_HALL, NULL},
    {"think.process", GW_CAP_KIND_FWD, NULL},
    {"sched.dag_submit", GW_CAP_KIND_FWD, NULL},
    {"cupolas.vault_store", GW_CAP_KIND_FWD, NULL},
    /* policy.* 统一 RPC 面（外部 namespace policy → cupolas_d） */
    {"policy.load", GW_CAP_KIND_FWD, "cupolas"},
    {"policy.activate", GW_CAP_KIND_FWD, "cupolas"},
    {"policy.rollback", GW_CAP_KIND_FWD, "cupolas"},
    {"policy.status", GW_CAP_KIND_FWD, "cupolas"},
    {"monit.heartbeat", GW_CAP_KIND_FWD, NULL},
    {"a2a.send_message", GW_CAP_KIND_FWD, NULL},
};

static int key_caps_check(void)
{
    for (size_t i = 0; i < sizeof(k_key_caps) / sizeof(k_key_caps[0]); ++i) {
        const gw_cap_t *c = gw_cap_find(k_key_caps[i].cap);
        const char *ns = k_key_caps[i].ns;

        if (!c || c->kind != k_key_caps[i].kind || (ns && strcmp(c->ns, ns) != 0)) {
            TEST_FAIL(k_key_caps[i].cap);
            return 0;
        }
    }
    return 1;
}

static void test_key_caps(void)
{
    TEST_BEGIN("key caps present with right kinds");
    if (!key_caps_check())
        return;
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
    /* 策略演化写操作管理员级，读操作默认放行 */
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.load"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.activate"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("policy.rollback"), "cap:cupolas.admin") == 0);
    ASSERT_TRUE(gw_cap_perm_for("policy.status") == NULL);
    /* 日常核心链路能力：无额外权限要求（默认放行） */
    ASSERT_TRUE(gw_cap_perm_for("llm.complete") == NULL);
    ASSERT_TRUE(gw_cap_perm_for("think.process") == NULL);
    ASSERT_TRUE(gw_cap_perm_for("mem.write") == NULL);
    /* agent.run 为外部主体可驱动的最高敏执行入口，
     * 独立权限 cap:agent.run 门禁；agent.cancel 归既有 cap:agent.control。
     * 旧契约「agent.run 默认放行」已废除——未显式 allow 时 PEP fail-closed。 */
    ASSERT_TRUE(strcmp(gw_cap_perm_for("agent.run"), "cap:agent.run") == 0);
    ASSERT_TRUE(strcmp(gw_cap_perm_for("agent.cancel"), "cap:agent.control") == 0);
    /* 未登记能力：NULL */
    ASSERT_TRUE(gw_cap_perm_for("unknown.cap") == NULL);
    ASSERT_TRUE(gw_cap_perm_for(NULL) == NULL);
    TEST_PASS();
}

static void test_ns_ownership(void)
{
    TEST_BEGIN("namespace exclusive ownership");
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

static void test_ns_policy(void)
{
    TEST_BEGIN("namespace policy table (manifest generated)");
    const gw_ns_pol_t *p = gw_ns_pol("llm");
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(strcmp(p->owner, "llm") == 0);
    ASSERT_TRUE(strcmp(p->sock, "llm") == 0);
    ASSERT_EQ_INT(p->route, 1);
    ASSERT_TRUE(p->prefix == NULL);
    /* 整编命名空间：消费方 ns 与宿主 daemon/sock 分离，wire 前缀非空 */
    p = gw_ns_pol("plugin");
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(strcmp(p->owner, "tool") == 0);
    ASSERT_TRUE(strcmp(p->prefix, "plugin") == 0);
    ASSERT_EQ_INT(p->route, 1);
    /* 不可路由命名空间：策略表内可见但派发拒绝 */
    p = gw_ns_pol("hall");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->route, 0);
    /* 尾点容忍（客户端 "llm." 形式）与未知/空值 fail-closed */
    p = gw_ns_pol("llm.");
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(strcmp(p->ns, "llm") == 0);
    ASSERT_TRUE(gw_ns_pol("nope") == NULL);
    ASSERT_TRUE(gw_ns_pol("") == NULL);
    ASSERT_TRUE(gw_ns_pol(NULL) == NULL);
    /* 槽位：合法项可定位，非法项 -1（限频表边界） */
    ASSERT_TRUE(gw_ns_slot(gw_ns_pol("llm")) >= 0);
    ASSERT_EQ_INT(gw_ns_slot(NULL), -1);
    TEST_PASS();
}

static void test_l2_methods(void)
{
    TEST_BEGIN("l2 passthrough methods");
    ASSERT_TRUE(gw_l2_method("shutdown") == 1);
    ASSERT_TRUE(gw_l2_method("get_stats") == 1);
    ASSERT_TRUE(gw_l2_method("health_check") == 1);
    ASSERT_TRUE(gw_l2_method("load") == 0);
    ASSERT_TRUE(gw_l2_method("") == 0);
    ASSERT_TRUE(gw_l2_method(NULL) == 0);
    TEST_PASS();
}

int main(void)
{
    printf("test_gateway_cap_registry: AIRY_HOME isolated to /tmp\n");
    test_key_caps();
    test_fail_closed();
    test_ns_timeout();
    test_contract_version();
    test_perm_requirements();
    test_ns_ownership();
    test_ns_policy();
    test_l2_methods();
    test_emit_event();
    printf("test_gateway_cap_registry: %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
