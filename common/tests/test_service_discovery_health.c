// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_health.c
 * @brief 服务发现健康域单元测试（heartbeat/update_health/update_connections）
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "../include/safe_string_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ==================== 20. sd_heartbeat - normal ==================== */
void test_sd_heartbeat_normal(void)
{
    TEST("sd_heartbeat - normal heartbeat");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "hb-svc", "test", &inst, "", "");

    int ret = sd_heartbeat(sd, "hb-svc", "inst-001");
    ASSERT(ret == 0, "heartbeat should succeed");

    sd_destroy(sd);
    PASS();
}

/* ==================== 21. sd_heartbeat - nonexistent ==================== */
void test_sd_heartbeat_nonexistent(void)
{
    TEST("sd_heartbeat - nonexistent service");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    int ret = sd_heartbeat(sd, "nonexistent", "inst-001");
    ASSERT(ret != 0, "heartbeat on nonexistent should fail");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "hb-svc", "test", &inst, "", "");

    ret = sd_heartbeat(sd, "hb-svc", "nonexistent-inst");
    ASSERT(ret != 0, "heartbeat on nonexistent instance should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 22. sd_update_health ==================== */
void test_sd_update_health(void)
{
    TEST("sd_update_health - mark unhealthy then healthy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    inst.healthy = true;
    sd_register(sd, "health-svc", "test", &inst, "", "");

    int ret = sd_update_health(sd, "health-svc", "inst-001", false);
    ASSERT(ret == 0, "mark unhealthy should succeed");

    sd_instance_t instances[8];
    uint32_t found = 0;
    sd_discover(sd, "health-svc", instances, 8, &found);
    ASSERT(found == 0, "unhealthy instance should not be discoverable");

    ret = sd_update_health(sd, "health-svc", "inst-001", true);
    ASSERT(ret == 0, "mark healthy should succeed");

    sd_discover(sd, "health-svc", instances, 8, &found);
    ASSERT(found == 1, "healthy instance should be discoverable again");

    sd_destroy(sd);
    PASS();
}

/* ==================== 23. sd_update_connections ==================== */
void test_sd_update_connections(void)
{
    TEST("sd_update_connections - update connections");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "conn-svc", "test", &inst, "", "");

    int ret = sd_update_connections(sd, "conn-svc", "inst-001", 42);
    ASSERT(ret == 0, "update connections should succeed");

    ret = sd_update_connections(sd, "nonexistent", "inst-001", 10);
    ASSERT(ret != 0, "update connections on nonexistent should fail");

    sd_destroy(sd);
    PASS();
}
