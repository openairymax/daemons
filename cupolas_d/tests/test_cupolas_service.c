// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cupolas_service.c
 * @brief cupolas_d 服务层真实性测试：daemon_cupolas_init 全量引导后
 *        直接驱动 cupolas_service_* 真实实现（permission/sanitizer/
 *        audit/vault/entitlements/network），严禁桩。
 *
 * 覆盖域：
 *  - 生命周期：create 非空 / destroy(NULL) 空安全 / audit_flush
 *  - 参数防线：各方法 NULL 入参 → AIRY_ERR_INVALID_PARAM
 *  - 权限引擎：allow/deny 双规则按 priority 裁决（deny 高优先级胜出，
 *    glob 跨 '/'），未登记主体 fail-closed
 *  - 输入净化：正常透传；危险输入净化或拒绝（fail-closed 二象性均合法）
 *  - 统计：get_stats_json 真实计数键（permission_checks/sanitize_count）
 *  - 凭据金库：store/retrieve/delete 全链路往返 + 越权 retrieve →
 *    AIRY_ERR_PERMISSION_DENIED
 *  - 授权书：扁平 YAML 装载、syscall/capability 白名单精确裁决、
 *    未装载 → AIRY_ERR_STATE_ERROR fail-closed、坏路径 → AIRY_ERR_UNKNOWN
 *  - 状态防线：daemon_security_shutdown 后 vault → AIRY_ERR_STATE_ERROR
 */

#include "cupolas_service.h"
#include "daemon_cupolas_bootstrap.h"
#include "daemon_security.h"
#include "error.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

#include "cupolas_vault_cred_type.h"

#define TEST_OK()                                                       \
    do {                                                                \
        printf("  %-46s ... ok\n", __func__);                           \
        return 0;                                                       \
    } while (0)

#define TEST_FAIL(msg)                                                  \
    do {                                                                \
        printf("  %-46s ... FAIL: %s\n", __func__, msg);                \
        return -1;                                                      \
    } while (0)

#define TEST_ASSERT(cond, msg)                                          \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("  %-46s ... FAIL: %s\n", __func__, msg);            \
            return -1;                                                  \
        }                                                               \
    } while (0)

static cupolas_service_t *g_svc;

static int t_svc_create(void)
{
    TEST_ASSERT(g_svc != NULL, "service not created");
    TEST_ASSERT(cupolas_service_audit_flush(g_svc) == AIRY_SUCCESS, "audit_flush");
    cupolas_service_destroy(NULL); /* 空销毁必须安全 */
    TEST_OK();
}

static int t_svc_null_args(void)
{
    cupolas_check_permission_result_t pr;
    cupolas_sanitize_result_t sr;
    cupolas_add_rule_result_t ar;
    cupolas_vault_store_result_t vr;
    cupolas_entitlements_load_result_t el;
    cupolas_entitlements_check_result_t ec;

    TEST_ASSERT(cupolas_service_check_permission(g_svc, NULL, &pr) ==
                AIRY_ERR_INVALID_PARAM, "check_permission");
    TEST_ASSERT(cupolas_service_sanitize(g_svc, NULL, &sr) ==
                AIRY_ERR_INVALID_PARAM, "sanitize");
    TEST_ASSERT(cupolas_service_add_rule(g_svc, NULL, &ar) ==
                AIRY_ERR_INVALID_PARAM, "add_rule");
    TEST_ASSERT(cupolas_service_vault_store(g_svc, NULL, &vr) ==
                AIRY_ERR_INVALID_PARAM, "vault_store");
    TEST_ASSERT(cupolas_service_entitlements_load(g_svc, NULL, &el) ==
                AIRY_ERR_INVALID_PARAM, "entitlements_load");
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, NULL, &ec) ==
                AIRY_ERR_INVALID_PARAM, "entitlements_check");
    TEST_OK();
}

static int t_svc_perm(void)
{
    cupolas_add_rule_params_t add = {.agent_id = "tperm", .action = "read"};
    cupolas_add_rule_result_t ar;
    cupolas_check_permission_params_t chk = {.action = "read"};
    cupolas_check_permission_result_t cr;

    add.resource = "/data/*";
    add.allow = 1;
    add.priority = 1;
    TEST_ASSERT(cupolas_service_add_rule(g_svc, &add, &ar) == AIRY_SUCCESS &&
                ar.added == 1, "add allow rule");

    chk.agent_id = "tperm";
    chk.resource = "/data/x";
    TEST_ASSERT(cupolas_service_check_permission(g_svc, &chk, &cr) == AIRY_SUCCESS &&
                cr.allowed == 1, "allow hit");

    add.resource = "/data/secret*";
    add.allow = 0;
    add.priority = 10;
    TEST_ASSERT(cupolas_service_add_rule(g_svc, &add, &ar) == AIRY_SUCCESS &&
                ar.added == 1, "add deny rule");

    chk.resource = "/data/secret/key";
    TEST_ASSERT(cupolas_service_check_permission(g_svc, &chk, &cr) == AIRY_SUCCESS &&
                cr.allowed == 0, "deny wins by priority");

    chk.agent_id = "ghost_user";
    chk.resource = "/data/x";
    TEST_ASSERT(cupolas_service_check_permission(g_svc, &chk, &cr) == AIRY_SUCCESS &&
                cr.allowed == 0, "unregistered fail-closed");
    TEST_OK();
}

static int t_svc_sanitize(void)
{
    cupolas_sanitize_params_t sp;
    cupolas_sanitize_result_t sr;
    int rc;

    sp.input = "hello world";
    rc = cupolas_service_sanitize(g_svc, &sp, &sr);
    TEST_ASSERT(rc == AIRY_SUCCESS, "plain input rejected");
    TEST_ASSERT(sr.sanitized && sr.sanitized[0], "empty output");
    cupolas_sanitize_result_free(&sr);

    sp.input = "<script>alert(1)</script>";
    rc = cupolas_service_sanitize(g_svc, &sp, &sr);
    if (rc == AIRY_SUCCESS) {
        TEST_ASSERT(sr.sanitized && strstr(sr.sanitized, "<script>") == NULL,
                    "script passed through");
        cupolas_sanitize_result_free(&sr);
    } else {
        TEST_ASSERT(rc == AIRY_ERR_PERMISSION_DENIED, "unexpected rc");
        TEST_ASSERT(sr.sanitized == NULL, "rejected input leaked");
    }

    /* 64KB 纯净长文本必须成功（1MB 输出上限内） */
    size_t n = 65536;
    char *big = AIRY_MALLOC(n + 1);
    TEST_ASSERT(big != NULL, "oom");
    memset(big, 'a', n);
    big[n] = '\0';
    sp.input = big;
    rc = cupolas_service_sanitize(g_svc, &sp, &sr);
    AIRY_FREE(big);
    TEST_ASSERT(rc == AIRY_SUCCESS, "big plain input rejected");
    cupolas_sanitize_result_free(&sr);
    TEST_OK();
}

static int t_svc_stats(void)
{
    char *j1 = cupolas_service_get_stats_json(g_svc);
    TEST_ASSERT(j1 != NULL, "stats null");
    TEST_ASSERT(strstr(j1, "permission_checks") && strstr(j1, "sanitize_count"),
                "real counter keys missing");
    AIRY_FREE(j1);

    cupolas_check_permission_params_t chk = {.agent_id = "tstat", .action = "read",
                                             .resource = "/none"};
    cupolas_check_permission_result_t cr;
    (void)cupolas_service_check_permission(g_svc, &chk, &cr);

    char *j2 = cupolas_service_get_stats_json(g_svc);
    TEST_ASSERT(j2 != NULL, "stats2 null");
    AIRY_FREE(j2);
    TEST_OK();
}

static int t_svc_vault(void)
{
    static const uint8_t secret[] = "sekrit-123"; /* 10 字节载荷 */
    const char *key = "svc_t_key";

    /* 防御跨运行残留：先删同名凭据（首次不存在，忽略结果） */
    cupolas_vault_delete_params_t dp = {.cred_id = key, .agent_id = "tva"};
    cupolas_vault_delete_result_t dr;
    (void)cupolas_service_vault_delete(g_svc, &dp, &dr);

    cupolas_vault_store_params_t sp = {.cred_id = key,
                                       .cred_type = CUPOLAS_VAULT_CRED_TOKEN,
                                       .data = secret,
                                       .data_len = sizeof(secret) - 1,
                                       .agent_id = "tva"};
    cupolas_vault_store_result_t sr;
    TEST_ASSERT(cupolas_service_vault_store(g_svc, &sp, &sr) == AIRY_SUCCESS &&
                sr.stored == 1, "store");

    cupolas_vault_retrieve_params_t rp = {.cred_id = key, .agent_id = "tva"};
    cupolas_vault_retrieve_result_t rr;
    TEST_ASSERT(cupolas_service_vault_retrieve(g_svc, &rp, &rr) == AIRY_SUCCESS,
                "authorized retrieve");
    TEST_ASSERT(rr.data_len == sizeof(secret) - 1 &&
                memcmp(rr.data, secret, sizeof(secret) - 1) == 0, "roundtrip mismatch");
    cupolas_vault_retrieve_result_free(&rr);

    rp.agent_id = "other";
    int rc = cupolas_service_vault_retrieve(g_svc, &rp, &rr);
    TEST_ASSERT(rc == AIRY_ERR_PERMISSION_DENIED && rr.data == NULL && rr.data_len == 0,
                "unauthorized retrieve not denied");
    cupolas_vault_retrieve_result_free(&rr);

    TEST_ASSERT(cupolas_service_vault_delete(g_svc, &dp, &dr) == AIRY_SUCCESS &&
                dr.deleted == 1, "delete");
    TEST_OK();
}

static int t_svc_entl(void)
{
    /* 未装载 → fail-closed 状态错误 */
    cupolas_entitlements_check_params_t ec = {.kind = "syscall", .param1 = "read"};
    cupolas_entitlements_check_result_t er;
    int rc = cupolas_service_entitlements_check(g_svc, &ec, &er);
    TEST_ASSERT(rc == AIRY_SUCCESS && er.allowed == 0 && er.err == AIRY_ERR_STATE_ERROR,
                "unloaded must be state-error fail-closed");

    /* 扁平 YAML：键名与 inline 数组格式均为解析器实锤语义 */
    char path[128];
#ifdef _WIN32
    snprintf(path, sizeof(path), "entl_%u.yaml", (unsigned)_getpid());
#else
    snprintf(path, sizeof(path), "/tmp/entl_%d.yaml", (int)getpid());
#endif
    FILE *f = fopen(path, "w");
    TEST_ASSERT(f != NULL, "tmp yaml open");
    const char *yaml = "agent_id: tentl\n"
                       "version: 1\n"
                       "allowed_syscalls: [read, write]\n"
                       "allowed_capabilities: [net.fs, net.tcp]\n";
    if (fputs(yaml, f) == EOF) {
        fclose(f);
        remove(path);
        TEST_FAIL("tmp yaml write");
    }
    fclose(f);

    cupolas_entitlements_load_params_t el = {.yaml_path = path};
    cupolas_entitlements_load_result_t lr;
    rc = cupolas_service_entitlements_load(g_svc, &el, &lr);
    remove(path);
    TEST_ASSERT(rc == AIRY_SUCCESS && lr.loaded == 1, "load");

    ec.kind = "syscall";
    ec.param1 = "read";
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, &ec, &er) == AIRY_SUCCESS &&
                er.allowed == 1, "syscall allow");
    ec.param1 = "exec";
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, &ec, &er) == AIRY_SUCCESS &&
                er.allowed == 0, "syscall not listed denied");
    ec.kind = "capability";
    ec.param1 = "net.fs";
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, &ec, &er) == AIRY_SUCCESS &&
                er.allowed == 1, "capability allow");
    ec.param1 = "disk.raw";
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, &ec, &er) == AIRY_SUCCESS &&
                er.allowed == 0, "capability not listed denied");
    ec.kind = "unknown";
    ec.param1 = "x";
    TEST_ASSERT(cupolas_service_entitlements_check(g_svc, &ec, &er) ==
                AIRY_ERR_INVALID_PARAM, "unknown kind");

    el.yaml_path = "/nonexistent/nope.yaml";
    TEST_ASSERT(cupolas_service_entitlements_load(g_svc, &el, &lr) == AIRY_ERR_UNKNOWN &&
                lr.loaded == 0, "bad path");
    TEST_OK();
}

static int t_svc_state_err(void)
{
    daemon_security_shutdown(); /* 不可逆：必须最后运行 */
    static const uint8_t secret[] = "x";
    cupolas_vault_store_params_t sp = {.cred_id = "post_shutdown",
                                       .cred_type = CUPOLAS_VAULT_CRED_TOKEN,
                                       .data = secret,
                                       .data_len = sizeof(secret) - 1,
                                       .agent_id = "tse"};
    cupolas_vault_store_result_t sr;
    TEST_ASSERT(cupolas_service_vault_store(g_svc, &sp, &sr) == AIRY_ERR_STATE_ERROR,
                "post-shutdown store must be state-error");
    TEST_OK();
}

int main(void)
{
    /* 全量引导（非 PEP 最小 guard）：vault + entitlements + network 真实
     * 初始化，测试驱动的是完整安全穹顶。 */
    if (daemon_cupolas_init("cupolas_d") != AIRY_OK) {
        printf("daemon_cupolas_init failed\n");
        return 1;
    }

    g_svc = cupolas_service_create(NULL);
    if (!g_svc) {
        printf("cupolas_service_create failed\n");
        return 1;
    }

    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        {"create/destroy", t_svc_create},
        {"null args", t_svc_null_args},
        {"permission rules", t_svc_perm},
        {"sanitize", t_svc_sanitize},
        {"stats", t_svc_stats},
        {"vault roundtrip", t_svc_vault},
        {"entitlements", t_svc_entl},
        {"post-shutdown state", t_svc_state_err},
    };

    int run = 0, pass = 0;
    size_t total = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < total; i++) {
        run++;
        printf("[%zu/%zu] %s\n", i + 1, total, tests[i].name);
        if (tests[i].fn() == 0)
            pass++;
    }

    cupolas_service_destroy(g_svc);
    daemon_cupolas_cleanup();
    printf("cupolas_service: %d/%d passed\n", pass, run);
    return pass == run ? 0 : 1;
}
