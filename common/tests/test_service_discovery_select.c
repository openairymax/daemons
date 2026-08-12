// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_select.c
 * @brief 服务发现 select_instance 域单元测试（round_robin/random/weighted/least_connection/least_load）
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "../include/safe_string_utils.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ==================== 17. sd_select_instance - round_robin ==================== */
void test_sd_select_instance_round_robin(void)
{
    TEST("sd_select_instance - round_robin strategy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");

    sd_register(sd, "lb-svc", "test", &inst1, "", "");
    sd_register(sd, "lb-svc", "test", &inst2, "", "");

    sd_instance_t selected;
    AIRY_MEMSET(&selected, 0, sizeof(selected));
    int ret = sd_select_instance(sd, "lb-svc", SD_LB_ROUND_ROBIN, &selected);
    ASSERT(ret == 0, "first select should succeed");
    ASSERT(selected.instance_id[0] != '\0', "should select an instance");

    ret = sd_select_instance(sd, "lb-svc", SD_LB_ROUND_ROBIN, &selected);
    ASSERT(ret == 0, "second select should succeed");

    sd_destroy(sd);
    PASS();
}

/* ==================== 18. sd_select_instance - random ==================== */
void test_sd_select_instance_random(void)
{
    TEST("sd_select_instance - random strategy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "rand-svc", "test", &inst, "", "");

    sd_instance_t selected;
    AIRY_MEMSET(&selected, 0, sizeof(selected));
    int ret = sd_select_instance(sd, "rand-svc", SD_LB_RANDOM, &selected);
    ASSERT(ret == 0, "random select should succeed");
    ASSERT(strcmp(selected.instance_id, "inst-001") == 0, "should select the only instance");

    sd_destroy(sd);
    PASS();
}

/* ==================== 19. sd_select_instance - nonexistent ==================== */
void test_sd_select_instance_nonexistent(void)
{
    TEST("sd_select_instance - nonexistent service");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t selected;
    int ret = sd_select_instance(sd, "nonexistent", SD_LB_ROUND_ROBIN, &selected);
    ASSERT(ret != 0, "select nonexistent should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 32. sd_select_instance - weighted ==================== */
void test_sd_select_instance_weighted(void)
{
    TEST("sd_select_instance - weighted strategy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    inst.weight = 100;
    sd_register(sd, "weight-svc", "test", &inst, "", "");

    sd_instance_t selected;
    AIRY_MEMSET(&selected, 0, sizeof(selected));
    int ret = sd_select_instance(sd, "weight-svc", SD_LB_WEIGHTED, &selected);
    ASSERT(ret == 0, "weighted select should succeed");
    ASSERT(strcmp(selected.instance_id, "inst-001") == 0, "should select the only instance");

    sd_destroy(sd);
    PASS();
}

/* ==================== 33. sd_select_instance - least_connection ==================== */
void test_sd_select_instance_least_connection(void)
{
    TEST("sd_select_instance - least_connection strategy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    inst1.active_connections = 10;
    sd_register(sd, "lc-svc", "test", &inst1, "", "");

    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");
    inst2.active_connections = 5;
    sd_register(sd, "lc-svc", "test", &inst2, "", "");

    sd_instance_t selected;
    AIRY_MEMSET(&selected, 0, sizeof(selected));
    int ret = sd_select_instance(sd, "lc-svc", SD_LB_LEAST_CONNECTION, &selected);
    ASSERT(ret == 0, "least_connection select should succeed");
    ASSERT(strcmp(selected.instance_id, "inst-002") == 0,
           "should select instance with fewer connections");

    sd_destroy(sd);
    PASS();
}

/* ==================== 34. sd_select_instance - least_load ==================== */
void test_sd_select_instance_least_load(void)
{
    TEST("sd_select_instance - least_load strategy");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst1 = make_instance("inst-001", "tcp://127.0.0.1:8080");
    inst1.max_connections = 100;
    inst1.active_connections = 10;
    sd_register(sd, "ll-svc", "test", &inst1, "", "");

    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:8081");
    inst2.max_connections = 100;
    inst2.active_connections = 50;
    sd_register(sd, "ll-svc", "test", &inst2, "", "");

    sd_instance_t selected;
    AIRY_MEMSET(&selected, 0, sizeof(selected));
    int ret = sd_select_instance(sd, "ll-svc", SD_LB_LEAST_LOAD, &selected);
    ASSERT(ret == 0, "least_load select should succeed");
    ASSERT(strcmp(selected.instance_id, "inst-001") == 0,
           "should select instance with lower load percentage");

    sd_destroy(sd);
    PASS();
}
