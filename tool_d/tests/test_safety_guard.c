// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_safety_guard.c
 * @brief tool_d SafetyGuard 桥接层单元测试：六类 guard 的通过/拒绝路径、
 *        计数语义、默认配置与入参边界。
 */

#include "approval/safety_guard_bridge.h"

#include "daemon_security.h"

/* AIRY_ERR_* 经 airy_memory.h -> error.h 引入 */
#include "airy_memory.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define AGENT "agent-guard"
#define TOOL  "fs_read"

static void cfg_all_on(safety_guard_bridge_config_t *cfg)
{
    sgb_cfg_defaults(cfg);
    cfg->agent_id = AGENT;
}

static void test_cfg_defaults(void)
{
    printf("  test_cfg_defaults...\n");

    safety_guard_bridge_config_t cfg;
    memset(&cfg, 0xA5, sizeof(cfg));
    sgb_cfg_defaults(&cfg);

    CHECK(cfg.enable_permission_guard);
    CHECK(cfg.enable_rate_limit_guard);
    CHECK(cfg.enable_content_filter);
    CHECK(cfg.enable_input_sanitization);
    CHECK(cfg.enable_resource_quota);
    CHECK(cfg.enable_audit_guard);
    CHECK(cfg.rate_limit_per_minute == 0);
    CHECK(cfg.max_params_size == 0);
    CHECK(cfg.denied_patterns == NULL);
    CHECK(cfg.agent_id && strcmp(cfg.agent_id, "unknown") == 0);

    sgb_cfg_defaults(NULL);

    printf("    PASSED\n");
}

static void test_null_args(void)
{
    printf("  test_null_args...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    meta.name = TOOL;

    CHECK(safety_guard_bridge_check_for_agent(NULL, AGENT, &meta, "{}", &res) ==
          AIRY_ERR_INVALID_PARAM);
    CHECK(safety_guard_bridge_check_for_agent(NULL, AGENT, NULL, "{}", &res) ==
          AIRY_ERR_INVALID_PARAM);

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, NULL, "{}", &res) ==
          AIRY_ERR_INVALID_PARAM);
    safety_guard_bridge_destroy(bridge);
    safety_guard_bridge_destroy(NULL);

    printf("    PASSED\n");
}

static void test_all_guards_pass(void)
{
    printf("  test_all_guards_pass...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    memset(&res, 0xA5, sizeof(res));
    int rc = safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{\"path\":\"/tmp/a\"}",
                                                 &res);
    CHECK(rc == 0);
    CHECK(res.permission_passed == 1);
    CHECK(res.rate_limit_passed == 1);
    CHECK(res.content_filter_passed == 1);
    CHECK(res.input_sanitized == 1);
    CHECK(res.resource_quota_passed == 1);
    CHECK(res.audit_recorded == 1);
    CHECK(res.denial_reason[0] == '\0');
    /* rate_limit_per_minute == 0 视为不限流，该 guard 不计入执行数 */
    CHECK(res.guard_chain_length == 5);
    CHECK(res.guards_executed == 5);

    /* result 为空时不得崩溃，且拒绝/通过判定不受影响 */
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{\"path\":\"/tmp/a\"}",
                                              NULL) == 0);

    /* 参数为空：内容过滤与消毒不参与计数 */
    memset(&res, 0, sizeof(res));
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, NULL, &res) == 0);
    CHECK(res.guard_chain_length == 3);
    CHECK(res.guards_executed == 3);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_permission_denied(void)
{
    printf("  test_permission_denied...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    meta.name = "fs_write";

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, "fs_write", false) == 0);

    memset(&res, 0, sizeof(res));
    int rc = safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{}", &res);
    CHECK(rc == AIRY_ERR_PERMISSION_DENIED);
    CHECK(res.permission_passed == 0);
    CHECK(strstr(res.denial_reason, "Permission denied") != NULL);
    CHECK(strstr(res.denial_reason, "fs_write") != NULL);
    /* 首个 guard 拒绝即返回，后续 guard 不再执行 */
    CHECK(res.guard_chain_length == 1);
    CHECK(res.guards_executed == 0);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_rate_limit(void)
{
    printf("  test_rate_limit...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    cfg.rate_limit_per_minute = 1;
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{}", &res) == 0);

    memset(&res, 0, sizeof(res));
    int rc = safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{}", &res);
    CHECK(rc == AIRY_ERR_BUSY);
    CHECK(res.rate_limit_passed == 0);
    CHECK(res.permission_passed == 1);
    CHECK(strstr(res.denial_reason, "Rate limit exceeded") != NULL);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_content_filter(void)
{
    printf("  test_content_filter...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    cfg.denied_patterns = "rm -rf, curl";
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    memset(&res, 0, sizeof(res));
    int rc = safety_guard_bridge_check_for_agent(bridge, AGENT, &meta,
                                                 "{\"cmd\":\"curl http://x\"}", &res);
    CHECK(rc == AIRY_ERR_SEC_VIOLATION);
    CHECK(res.content_filter_passed == 0);
    CHECK(strstr(res.denial_reason, "curl") != NULL);

    /* 未命中黑名单词则放行 */
    memset(&res, 0, sizeof(res));
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{\"cmd\":\"cat a.txt\"}",
                                              &res) == 0);
    CHECK(res.content_filter_passed == 1);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_max_params_size(void)
{
    printf("  test_max_params_size...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    cfg.max_params_size = 8;
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    memset(&res, 0, sizeof(res));
    int rc = safety_guard_bridge_check_for_agent(bridge, AGENT, &meta,
                                                 "{\"path\":\"/etc/passwd\"}", &res);
    CHECK(rc == AIRY_ERR_SEC_VIOLATION);
    CHECK(res.content_filter_passed == 0);
    CHECK(strstr(res.denial_reason, "exceeds max") != NULL);

    memset(&res, 0, sizeof(res));
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{}", &res) == 0);
    CHECK(res.content_filter_passed == 1);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_sanitize(void)
{
    printf("  test_sanitize...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    memset(&res, 0, sizeof(res));
    const char *dirty = "{\"a\":\"b\nc\"}";
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, dirty, &res) == 0);
    CHECK(res.input_sanitized == 1);
    CHECK(res.sanitized_params[0] != '\0');
    CHECK(strchr(res.sanitized_params, '\n') == NULL);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_agent_fallback(void)
{
    printf("  test_agent_fallback...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);
    CHECK(daemon_security_add_acl_rule(AGENT, TOOL, true) == 0);

    /* NULL 与空串均回落到 bridge 默认 agent */
    CHECK(safety_guard_bridge_check_for_agent(bridge, NULL, &meta, "{}", &res) == 0);
    CHECK(res.permission_passed == 1);
    CHECK(safety_guard_bridge_check_for_agent(bridge, "", &meta, "{}", &res) == 0);
    CHECK(res.permission_passed == 1);

    /* 其他 agent 无 ACL 条目：fail-closed */
    memset(&res, 0, sizeof(res));
    CHECK(safety_guard_bridge_check_for_agent(bridge, "agent-other", &meta, "{}", &res) ==
          AIRY_ERR_PERMISSION_DENIED);
    CHECK(res.permission_passed == 0);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

static void test_guards_disabled(void)
{
    printf("  test_guards_disabled...\n");

    safety_guard_bridge_config_t cfg;
    tool_metadata_t meta = {0};
    safety_guard_bridge_result_t res;

    cfg_all_on(&cfg);
    cfg.enable_permission_guard = false;
    cfg.enable_rate_limit_guard = false;
    cfg.enable_content_filter = false;
    cfg.enable_input_sanitization = false;
    cfg.enable_resource_quota = false;
    cfg.enable_audit_guard = false;
    meta.name = TOOL;

    safety_guard_bridge_t *bridge = safety_guard_bridge_create(&cfg);
    CHECK(bridge != NULL);

    memset(&res, 0xA5, sizeof(res));
    CHECK(safety_guard_bridge_check_for_agent(bridge, AGENT, &meta, "{}", &res) == 0);
    CHECK(res.permission_passed == 0);
    CHECK(res.rate_limit_passed == 0);
    CHECK(res.content_filter_passed == 0);
    CHECK(res.input_sanitized == 0);
    CHECK(res.resource_quota_passed == 0);
    CHECK(res.audit_recorded == 0);
    CHECK(res.guard_chain_length == 0);
    CHECK(res.guards_executed == 0);

    safety_guard_bridge_destroy(bridge);

    printf("    PASSED\n");
}

int main(void)
{
    printf("tool_d safety guard bridge tests\n");

    CHECK(daemon_security_init(NULL, NULL) == 0);

    test_cfg_defaults();
    test_null_args();
    test_all_guards_pass();
    test_permission_denied();
    test_rate_limit();
    test_content_filter();
    test_max_params_size();
    test_sanitize();
    test_agent_fallback();
    test_guards_disabled();

    daemon_security_shutdown();

    if (g_failures > 0) {
        printf("FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
