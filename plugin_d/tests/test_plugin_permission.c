// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_plugin_permission.c
 * @brief plugin_d 权限模块单元测试：manifest 权限到 Cupolas 守卫类型映射与校验。
 */

#include "plugin_permission.h"
#include "safety_guard.h"

/* AIRY_ERR_* 经 airy_memory.h -> error.h 引入 */
#include "airy_memory.h"

#include <assert.h>
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

static void test_map_all_permissions(void)
{
    printf("  test_map_all_permissions...\n");

    struct {
        const char *perm;
        safety_guard_type_t expected;
    } cases[] = {
        {"file_read", SAFETY_GUARD_FILE_READ},
        {"file_write", SAFETY_GUARD_FILE_WRITE},
        {"network_outbound", SAFETY_GUARD_NETWORK},
        {"network_inbound", SAFETY_GUARD_NETWORK},
        {"tool_execute", SAFETY_GUARD_TOOL_EXEC},
        {"memory_access", SAFETY_GUARD_MEMORY},
        {"hook_register", SAFETY_GUARD_HOOK},
        {"system_call", SAFETY_GUARD_SYSTEM},
        {"process_spawn", SAFETY_GUARD_PROCESS},
        {"ipc_connect", SAFETY_GUARD_IPC},
        {"service_discovery", SAFETY_GUARD_SERVICE_DISCOVERY},
        {"config_read", SAFETY_GUARD_CONFIG},
        {"config_write", SAFETY_GUARD_CONFIG},
        {"log_write", SAFETY_GUARD_LOGGING},
        {"metrics_export", SAFETY_GUARD_METRICS},
        {"audit_trigger", SAFETY_GUARD_AUDIT},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        safety_guard_type_t guard = (safety_guard_type_t)-1;
        int ret = plugin_permission_map_to_guard(cases[i].perm, &guard);
        CHECK(ret == 0);
        CHECK(guard == cases[i].expected);
    }

    printf("    PASSED (%zu cases)\n", sizeof(cases) / sizeof(cases[0]));
}

static void test_map_invalid(void)
{
    printf("  test_map_invalid...\n");

    safety_guard_type_t guard = (safety_guard_type_t)-1;

    CHECK(plugin_permission_map_to_guard(NULL, &guard) == AIRY_ERR_INVALID_PARAM);
    CHECK(plugin_permission_map_to_guard("file_read", NULL) == AIRY_ERR_INVALID_PARAM);
    CHECK(plugin_permission_map_to_guard("no_such_perm", &guard) == AIRY_ERR_NOT_SUPPORTED);
    CHECK(plugin_permission_map_to_guard("", &guard) == AIRY_ERR_NOT_SUPPORTED);

    printf("    PASSED\n");
}

static void test_check_no_permissions_strict(void)
{
    printf("  test_check_no_permissions_strict...\n");

    plugin_permission_destroy();
    plugin_permission_config_t cfg = {
        .enable_strict_mode = true,
        .enable_audit_log = false,
    };
    CHECK(plugin_permission_init(&cfg) == 0);

    char denied[128] = {0};
    plugin_permission_result_t res =
        plugin_permission_check(NULL, 0, "test-plugin", denied, sizeof(denied));
    CHECK(res == PLUGIN_PERM_DENIED);
    CHECK(strstr(denied, "no permissions") != NULL);

    printf("    PASSED\n");
}

static void test_check_no_permissions_lenient(void)
{
    printf("  test_check_no_permissions_lenient...\n");

    plugin_permission_destroy();
    plugin_permission_config_t cfg = {
        .enable_strict_mode = false,
        .enable_audit_log = false,
    };
    CHECK(plugin_permission_init(&cfg) == 0);

    plugin_permission_result_t res = plugin_permission_check(NULL, 0, "test-plugin", NULL, 0);
    CHECK(res == PLUGIN_PERM_ALLOWED);

    printf("    PASSED\n");
}

static void test_check_unknown_permission_strict(void)
{
    printf("  test_check_unknown_permission_strict...\n");

    plugin_permission_destroy();
    plugin_permission_config_t cfg = {
        .enable_strict_mode = true,
        .enable_audit_log = false,
    };
    CHECK(plugin_permission_init(&cfg) == 0);

    const char permissions[][64] = {"file_read", "mystery_perm"};
    char denied[128] = {0};
    plugin_permission_result_t res =
        plugin_permission_check(permissions, 2, "test-plugin", denied, sizeof(denied));
    CHECK(res == PLUGIN_PERM_UNKNOWN);
    CHECK(strstr(denied, "mystery_perm") != NULL);
    CHECK(strstr(denied, "file_read") == NULL);

    printf("    PASSED\n");
}

static void test_check_valid_permissions(void)
{
    printf("  test_check_valid_permissions...\n");

    plugin_permission_destroy();
    plugin_permission_config_t cfg = {
        .enable_strict_mode = true,
        .enable_audit_log = false,
    };
    CHECK(plugin_permission_init(&cfg) == 0);

    const char permissions[][64] = {"file_read", "network_outbound", "tool_execute"};
    char denied[128] = {0};
    plugin_permission_result_t res =
        plugin_permission_check(permissions, 3, "test-plugin", denied, sizeof(denied));
    /* 新守卫上下文未注册任何守卫，默认放行已知权限 */
    CHECK(res == PLUGIN_PERM_ALLOWED);

    /* 空权限项被跳过 */
    const char mixed[][64] = {"file_read", "", "tool_execute"};
    res = plugin_permission_check(mixed, 3, "test-plugin", denied, sizeof(denied));
    CHECK(res == PLUGIN_PERM_ALLOWED);

    printf("    PASSED\n");
}

static void test_description(void)
{
    printf("  test_description...\n");

    CHECK(plugin_permission_description("file_read") != NULL);
    CHECK(strcmp(plugin_permission_description("file_read"), "Read files from the filesystem") == 0);
    CHECK(plugin_permission_description(NULL) != NULL);
    CHECK(strcmp(plugin_permission_description("bogus"), "unknown permission") == 0);

    printf("    PASSED\n");
}

static void test_list_supported(void)
{
    printf("  test_list_supported...\n");

    CHECK(plugin_permission_list_supported(NULL, NULL) == AIRY_ERR_INVALID_PARAM);

    char **perms = NULL;
    size_t count = 0;
    CHECK(plugin_permission_list_supported(&perms, &count) == 0);
    CHECK(count == 16);
    CHECK(perms != NULL);

    if (perms) {
        bool found_read = false;
        for (size_t i = 0; i < count; i++) {
            if (perms[i] && strcmp(perms[i], "file_read") == 0)
                found_read = true;
            AIRY_FREE(perms[i]);
        }
        CHECK(found_read);
        AIRY_FREE(perms);
    }

    printf("    PASSED\n");
}

int main(void)
{
    printf("plugin_d permission tests\n");

    test_map_all_permissions();
    test_map_invalid();
    test_check_no_permissions_strict();
    test_check_no_permissions_lenient();
    test_check_unknown_permission_strict();
    test_check_valid_permissions();
    test_description();
    test_list_supported();

    plugin_permission_destroy();

    if (g_failures > 0) {
        printf("FAILED: %d assertion(s)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
