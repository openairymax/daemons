// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_lifecycle.c
 * @brief 服务发现生命周期域单元测试（create/destroy/start/stop/register/deregister）
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "safe_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ==================== 1. sd_create_default_config ==================== */
void test_sd_create_default_config(void)
{
    TEST("sd_create_default_config - verify defaults");
    sd_config_t cfg = sd_create_default_config();
    ASSERT(cfg.heartbeat_interval_ms == SD_DEFAULT_HEARTBEAT_MS, "default heartbeat_interval_ms");
    ASSERT(cfg.expire_timeout_ms == SD_DEFAULT_EXPIRE_MS, "default expire_timeout_ms");
    ASSERT(cfg.default_lb_strategy == SD_LB_ROUND_ROBIN,
           "default lb strategy should be ROUND_ROBIN");
    ASSERT(cfg.enable_auto_expire == true, "auto expire should be enabled by default");
    ASSERT(cfg.enable_health_propagation == true,
           "health propagation should be enabled by default");
    ASSERT(cfg.shm_size == 1024 * 1024, "default shm_size should be 1MB");
    ASSERT(strcmp(cfg.shm_name, SD_SHM_NAME) == 0, "default shm_name should match SD_SHM_NAME");
    PASS();
}

/* ==================== 2. sd_create(NULL) ==================== */
void test_sd_create_null_config(void)
{
    TEST("sd_create(NULL) - create with defaults");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "sd_create(NULL) should succeed");
    sd_destroy(sd);
    PASS();
}

/* ==================== 3. sd_create(config) ==================== */
void test_sd_create_with_config(void)
{
    TEST("sd_create(config) - create with custom config");
    sd_config_t cfg = sd_create_default_config();
    cfg.heartbeat_interval_ms = 5000;
    cfg.expire_timeout_ms = 15000;
    cfg.default_lb_strategy = SD_LB_RANDOM;
    cfg.enable_auto_expire = false;
    cfg.enable_health_propagation = false;

    service_discovery_t sd = sd_create(&cfg);
    ASSERT(sd != NULL, "sd_create with config should succeed");
    sd_destroy(sd);
    PASS();
}

/* ==================== 4. sd_destroy(NULL) ==================== */
void test_sd_destroy_null(void)
{
    TEST("sd_destroy(NULL) - safe null handling");
    sd_destroy(NULL);
    PASS();
}

/* ==================== 5. sd_start / sd_stop / sd_is_running lifecycle ==================== */
void test_sd_lifecycle(void)
{
    TEST("sd_start/sd_stop/sd_is_running lifecycle");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    ASSERT(sd_is_running(sd) == false, "should not be running before start");

    int ret = sd_start(sd);
    ASSERT(ret == 0, "sd_start should succeed");
    ASSERT(sd_is_running(sd) == true, "should be running after start");

    ret = sd_stop(sd);
    ASSERT(ret == 0, "sd_stop should succeed");
    ASSERT(sd_is_running(sd) == false, "should not be running after stop");

    sd_destroy(sd);
    PASS();
}

/* ==================== 6. sd_start idempotent ==================== */
void test_sd_start_idempotent(void)
{
    TEST("sd_start on already started - idempotent");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    int ret = sd_start(sd);
    ASSERT(ret == 0, "first start");
    ASSERT(sd_is_running(sd) == true, "should be running");

    ret = sd_start(sd);
    ASSERT(ret == 0, "second start should be idempotent");
    ASSERT(sd_is_running(sd) == true, "should still be running");

    sd_destroy(sd);
    PASS();
}

/* ==================== 7. sd_stop(NULL) ==================== */
void test_sd_stop_null(void)
{
    TEST("sd_stop(NULL) - rejected");
    int ret = sd_stop(NULL);
    ASSERT(ret != 0, "sd_stop(NULL) should be rejected");
    PASS();
}

/* ==================== 8. sd_register - normal ==================== */
void test_sd_register_normal(void)
{
    TEST("sd_register - normal registration");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    int ret =
        sd_register(sd, "auth-service", "auth", &inst, "critical,prod", "db-service,log-service");
    ASSERT(ret == 0, "registration should succeed");

    sd_service_count(sd);
    ASSERT(sd_service_count(sd) == 1, "service count should be 1");

    sd_destroy(sd);
    PASS();
}

/* ==================== 9. sd_register - NULL parameter validation ==================== */
void test_sd_register_null_params(void)
{
    TEST("sd_register - NULL parameter validation");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");
    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");

    int ret = sd_register(NULL, "svc", "type", &inst, "tags", "deps");
    ASSERT(ret != 0, "null sd should be rejected");

    ret = sd_register(sd, NULL, "type", &inst, "tags", "deps");
    ASSERT(ret != 0, "null service_name should be rejected");

    ret = sd_register(sd, "svc", NULL, &inst, "tags", "deps");
    ASSERT(ret != 0, "null service_type should be rejected");

    ret = sd_register(sd, "svc", "type", NULL, "tags", "deps");
    ASSERT(ret != 0, "null instance should be rejected");

    sd_destroy(sd);
    PASS();
}

/* ==================== 10. sd_deregister - normal ==================== */
void test_sd_deregister_normal(void)
{
    TEST("sd_deregister - normal deregistration");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    int ret = sd_register(sd, "auth-service", "auth", &inst, "", "");
    ASSERT(ret == 0, "register");
    ASSERT(sd_service_count(sd) == 1, "count after register");

    ret = sd_deregister(sd, "auth-service", "inst-001");
    ASSERT(ret == 0, "deregister should succeed");

    sd_destroy(sd);
    PASS();
}

/* ==================== 11. sd_deregister - nonexistent service ==================== */
void test_sd_deregister_nonexistent(void)
{
    TEST("sd_deregister - nonexistent service");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    int ret = sd_deregister(sd, "nonexistent", "inst-001");
    ASSERT(ret != 0, "deregister nonexistent should fail");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "real-svc", "type", &inst, "", "");

    ret = sd_deregister(sd, "real-svc", "nonexistent-inst");
    ASSERT(ret != 0, "deregister nonexistent instance should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 12. sd_deregister_all ==================== */
void test_sd_deregister_all(void)
{
    TEST("sd_deregister_all - deregister all instances");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");

    sd_register(sd, "web-service", "web", &inst1, "", "");
    sd_register(sd, "web-service", "web", &inst2, "", "");

    uint32_t found = 0;
    sd_instance_t discovered[8];
    sd_discover(sd, "web-service", discovered, 8, &found);
    ASSERT(found == 2, "should have 2 instances before deregister_all");

    int ret = sd_deregister_all(sd, "web-service");
    ASSERT(ret == 0, "deregister_all should succeed");

    sd_discover(sd, "web-service", discovered, 8, &found);
    ASSERT(found == 0, "should have 0 instances after deregister_all");

    sd_destroy(sd);
    PASS();
}
