// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_discover.c
 * @brief 服务发现 discover 域单元测试（discover/discover_by_type/discover_by_tags/依赖查询）
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "safe_string_utils.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ==================== 13. sd_discover - discover registered instances ==================== */
void test_sd_discover_normal(void)
{
    TEST("sd_discover - discover registered instances");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");

    sd_register(sd, "api-gateway", "gateway", &inst1, "", "");
    sd_register(sd, "api-gateway", "gateway", &inst2, "", "");

    sd_instance_t instances[8];
    uint32_t found = 0;
    int ret = sd_discover(sd, "api-gateway", instances, 8, &found);
    ASSERT(ret == 0, "discover should succeed");
    ASSERT(found == 2, "should find 2 instances");

    sd_destroy(sd);
    PASS();
}

/* ==================== 14. sd_discover - nonexistent service ==================== */
void test_sd_discover_nonexistent(void)
{
    TEST("sd_discover - nonexistent service");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t instances[8];
    uint32_t found = 99;
    int ret = sd_discover(sd, "nonexistent", instances, 8, &found);
    ASSERT(ret != 0, "discover nonexistent should return error");
    ASSERT(found == 0, "found count should be 0 for nonexistent");

    sd_destroy(sd);
    PASS();
}

/* ==================== 15. sd_discover_by_type ==================== */
void test_sd_discover_by_type(void)
{
    TEST("sd_discover_by_type - find services by type");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:9090");

    sd_register(sd, "auth-svc-1", "auth", &inst1, "", "");
    sd_register(sd, "auth-svc-2", "auth", &inst2, "", "");
    sd_register(sd, "web-svc-1", "web", &inst1, "", "");

    sd_service_entry_t entries[8];
    uint32_t found = 0;
    int ret = sd_discover_by_type(sd, "auth", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_type should succeed");
    ASSERT(found == 2, "should find 2 auth services");

    ret = sd_discover_by_type(sd, "web", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_type web should succeed");
    ASSERT(found == 1, "should find 1 web service");

    ret = sd_discover_by_type(sd, "nonexistent", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_type nonexistent should succeed with 0");
    ASSERT(found == 0, "should find 0 for unknown type");

    sd_destroy(sd);
    PASS();
}

/* ==================== 16. sd_discover_by_tags ==================== */
void test_sd_discover_by_tags(void)
{
    TEST("sd_discover_by_tags - find services by tags");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");

    sd_register(sd, "svc-a", "typeA", &inst, "critical,frontend", "");
    sd_register(sd, "svc-b", "typeB", &inst, "critical,backend", "");
    sd_register(sd, "svc-c", "typeC", &inst, "optional,frontend", "");

    sd_service_entry_t entries[8];
    uint32_t found = 0;
    int ret = sd_discover_by_tags(sd, "critical", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_tags should succeed");
    ASSERT(found == 2, "should find 2 critical services");

    ret = sd_discover_by_tags(sd, "frontend", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_tags frontend should succeed");
    ASSERT(found == 2, "should find 2 frontend services");

    ret = sd_discover_by_tags(sd, "backend", entries, 8, &found);
    ASSERT(ret == 0, "discover_by_tags backend should succeed");
    ASSERT(found == 1, "should find 1 backend service");

    sd_destroy(sd);
    PASS();
}

/* ==================== 24. sd_get_dependencies ==================== */
void test_sd_get_dependencies(void)
{
    TEST("sd_get_dependencies - retrieve dependencies");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "dep-svc", "test", &inst, "", "db-service,log-service,cache");

    char deps[SD_MAX_DEPS_LEN];
    AIRY_MEMSET(deps, 0, sizeof(deps));
    int ret = sd_get_dependencies(sd, "dep-svc", deps, sizeof(deps));
    ASSERT(ret == 0, "get_dependencies should succeed");
    ASSERT(strstr(deps, "db-service") != NULL, "should contain db-service");
    ASSERT(strstr(deps, "log-service") != NULL, "should contain log-service");
    ASSERT(strstr(deps, "cache") != NULL, "should contain cache");

    ret = sd_get_dependencies(sd, "nonexistent", deps, sizeof(deps));
    ASSERT(ret != 0, "get_dependencies on nonexistent should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 25. sd_check_dependencies ==================== */
void test_sd_check_dependencies(void)
{
    TEST("sd_check_dependencies - check dependency health");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "app-svc", "app", &inst, "", "db-service,cache-service");

    sd_instance_t db_inst = make_instance("db-001", "tcp://127.0.0.1:5432");
    sd_register(sd, "db-service", "db", &db_inst, "", "");

    char missing[SD_MAX_DEPS_LEN];
    AIRY_MEMSET(missing, 0, sizeof(missing));
    int ret = sd_check_dependencies(sd, "app-svc", missing, sizeof(missing));
    ASSERT(ret != 0, "check_deps should indicate missing deps");

    ret = sd_check_dependencies(sd, "app-svc", NULL, 0);
    ASSERT(ret != 0, "check_deps with NULL buffer should still report missing");

    sd_instance_t cache_inst = make_instance("cache-001", "tcp://127.0.0.1:6379");
    sd_register(sd, "cache-service", "cache", &cache_inst, "", "");

    AIRY_MEMSET(missing, 0, sizeof(missing));
    ret = sd_check_dependencies(sd, "app-svc", missing, sizeof(missing));
    ASSERT(ret == 0, "check_deps should succeed when all deps are present");

    sd_destroy(sd);
    PASS();
}

/* ==================== 35. sd_discover with max_count limit ==================== */
void test_sd_discover_max_count(void)
{
    TEST("sd_discover - max_count limit");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");
    sd_instance_t inst3 = make_instance("inst-003", "tcp://127.0.0.1:8082");

    sd_register(sd, "limit-svc", "test", &inst1, "", "");
    sd_register(sd, "limit-svc", "test", &inst2, "", "");
    sd_register(sd, "limit-svc", "test", &inst3, "", "");

    sd_instance_t instances[2];
    uint32_t found = 0;
    int ret = sd_discover(sd, "limit-svc", instances, 2, &found);
    ASSERT(ret == 0, "discover should succeed");
    ASSERT(found == 2, "should return at most 2 instances");

    sd_destroy(sd);
    PASS();
}

/* ==================== 38. sd_discover returns only healthy ==================== */
void test_sd_discover_only_healthy(void)
{
    TEST("sd_discover - returns only healthy instances");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst_healthy = make_instance("inst-h", "tcp://127.0.0.1:8080");
    inst_healthy.healthy = true;

    sd_instance_t inst_unhealthy = make_instance("inst-u", "tcp://127.0.0.1:8081");
    inst_unhealthy.healthy = false;

    sd_register(sd, "mixed-svc", "test", &inst_healthy, "", "");
    sd_register(sd, "mixed-svc", "test", &inst_unhealthy, "", "");

    sd_instance_t instances[8];
    uint32_t found = 0;
    int ret = sd_discover(sd, "mixed-svc", instances, 8, &found);
    ASSERT(ret == 0, "discover should succeed");
    ASSERT(found == 1, "should return only healthy instances");
    ASSERT(strcmp(instances[0].instance_id, "inst-h") == 0,
           "returned instance should be the healthy one");

    sd_destroy(sd);
    PASS();
}
