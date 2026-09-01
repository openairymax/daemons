// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_misc.c
 * @brief 服务发现杂项域单元测试（callback/stats/count/stress/null safety）
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "safe_string_utils.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ==================== 26. sd_register_event_callback ==================== */
void test_sd_register_event_callback(void)
{
    TEST("sd_register_event_callback - register callback");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    callback_fired = 0;
    last_event = 0;

    int ret = sd_register_event_callback(sd, event_callback, NULL);
    ASSERT(ret == 0, "register callback should succeed");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "cb-svc", "test", &inst, "", "");
    ASSERT(callback_fired >= 1, "callback should have been fired for register");
    ASSERT(last_event == SD_EVENT_REGISTERED, "should be REGISTERED event");

    int prev_fired = callback_fired;
    sd_deregister(sd, "cb-svc", "inst-001");
    ASSERT(callback_fired > prev_fired, "callback should fire for deregister");
    ASSERT(last_event == SD_EVENT_DEREGISTERED, "should be DEREGISTERED event");

    ret = sd_register_event_callback(NULL, event_callback, NULL);
    ASSERT(ret != 0, "register callback on NULL sd should fail");

    ret = sd_register_event_callback(sd, NULL, NULL);
    ASSERT(ret != 0, "register NULL callback should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 27. sd_get_stats ==================== */
void test_sd_get_stats(void)
{
    TEST("sd_get_stats - retrieve stats");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "stats-svc", "test", &inst, "", "");

    sd_stats_t stats;
    AIRY_MEMSET(&stats, 0, sizeof(stats));
    int ret = sd_get_stats(sd, &stats);
    ASSERT(ret == 0, "get_stats should succeed");
    ASSERT(stats.registrations >= 1, "should have at least 1 registration");
    ASSERT(stats.active_services >= 1, "should have at least 1 active service");
    ASSERT(stats.active_instances >= 1, "should have at least 1 active instance");

    ret = sd_get_stats(NULL, &stats);
    ASSERT(ret != 0, "get_stats on NULL should fail");

    ret = sd_get_stats(sd, NULL);
    ASSERT(ret != 0, "get_stats with NULL stats should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 28. sd_service_count ==================== */
void test_sd_service_count(void)
{
    TEST("sd_service_count - service count tracking");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    uint32_t count = sd_service_count(NULL);
    ASSERT(count == 0, "NULL sd should return 0");

    count = sd_service_count(sd);
    ASSERT(count == 0, "initial count should be 0");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "svc1", "type1", &inst, "", "");
    count = sd_service_count(sd);
    ASSERT(count == 1, "count should be 1 after first registration");

    sd_instance_t inst2 = make_instance("inst-002", "tcp://127.0.0.1:9090");
    sd_register(sd, "svc2", "type2", &inst2, "", "");
    count = sd_service_count(sd);
    ASSERT(count == 2, "count should be 2 after second registration");

    sd_deregister_all(sd, "svc1");
    count = sd_service_count(sd);
    ASSERT(count == 2, "deregister_all should not remove service entry");

    sd_destroy(sd);
    PASS();
}

/* ==================== 29. sd_lb_strategy_to_string ==================== */
void test_sd_lb_strategy_to_string(void)
{
    TEST("sd_lb_strategy_to_string - all strategies");
    const char *s;

    s = sd_lb_strategy_to_string(SD_LB_ROUND_ROBIN);
    ASSERT(s != NULL && strcmp(s, "ROUND_ROBIN") == 0, "ROUND_ROBIN string");

    s = sd_lb_strategy_to_string(SD_LB_WEIGHTED);
    ASSERT(s != NULL && strcmp(s, "WEIGHTED") == 0, "WEIGHTED string");

    s = sd_lb_strategy_to_string(SD_LB_LEAST_CONNECTION);
    ASSERT(s != NULL && strcmp(s, "LEAST_CONNECTION") == 0, "LEAST_CONNECTION string");

    s = sd_lb_strategy_to_string(SD_LB_RANDOM);
    ASSERT(s != NULL && strcmp(s, "RANDOM") == 0, "RANDOM string");

    s = sd_lb_strategy_to_string(SD_LB_LEAST_LOAD);
    ASSERT(s != NULL && strcmp(s, "LEAST_LOAD") == 0, "LEAST_LOAD string");

    s = sd_lb_strategy_to_string((sd_lb_strategy_t)999);
    ASSERT(s != NULL && strcmp(s, "UNKNOWN") == 0, "invalid strategy should return UNKNOWN");

    s = sd_lb_strategy_to_string((sd_lb_strategy_t)-1);
    ASSERT(s != NULL && strcmp(s, "UNKNOWN") == 0, "negative strategy should return UNKNOWN");

    PASS();
}

/* ==================== 30. Multiple services and instances stress ==================== */
void test_sd_multiple_services_stress(void)
{
    TEST("Multiple services and instances stress");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    const int num_services = 5;
    const int num_instances = 4;
    char svc_name[SD_MAX_NAME_LEN];
    char inst_id[SD_MAX_NAME_LEN];
    char endpoint[SD_MAX_ENDPOINT_LEN];

    for (int s = 0; s < num_services; s++) {
        snprintf(svc_name, sizeof(svc_name), "stress-svc-%d", s);
        for (int i = 0; i < num_instances; i++) {
            snprintf(inst_id, sizeof(inst_id), "inst-%d-%d", s, i);
            snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%d", 8000 + s * 100 + i);
            sd_instance_t inst = make_instance(inst_id, endpoint);
            int ret = sd_register(sd, svc_name, "stress", &inst, "", "");
            ASSERT(ret == 0, "register should succeed");
        }
    }

    uint32_t count = sd_service_count(sd);
    ASSERT(count == (uint32_t)num_services, "should have all services");

    sd_instance_t instances[SD_MAX_INSTANCES];
    uint32_t found = 0;
    int ret = sd_discover(sd, "stress-svc-0", instances, SD_MAX_INSTANCES, &found);
    ASSERT(ret == 0, "discover should succeed");
    ASSERT(found == (uint32_t)num_instances, "should find all instances");

    sd_service_entry_t entries[SD_MAX_SERVICES];
    ret = sd_discover_by_type(sd, "stress", entries, SD_MAX_SERVICES, &found);
    ASSERT(ret == 0, "discover_by_type should succeed");
    ASSERT(found == (uint32_t)num_services, "should find all stress services");

    for (int s = 0; s < num_services; s++) {
        snprintf(svc_name, sizeof(svc_name), "stress-svc-%d", s);
        sd_deregister_all(sd, svc_name);
    }

    sd_destroy(sd);
    PASS();
}

/* ==================== 31. All null parameter safety checks ==================== */
void test_sd_null_parameter_safety(void)
{
    TEST("All null parameter safety checks for each API");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    ASSERT(sd_start(NULL) != 0, "sd_start(NULL) rejected");
    ASSERT(sd_stop(NULL) != 0, "sd_stop(NULL) rejected");
    ASSERT(sd_is_running(NULL) == false, "sd_is_running(NULL) returns false");

    sd_instance_t inst = make_instance("inst-x", "tcp://1.2.3.4:80");
    ASSERT(sd_register(NULL, "s", "t", &inst, "", "") != 0, "sd_register null sd");
    ASSERT(sd_register(sd, NULL, "t", &inst, "", "") != 0, "sd_register null name");
    ASSERT(sd_register(sd, "s", NULL, &inst, "", "") != 0, "sd_register null type");
    ASSERT(sd_register(sd, "s", "t", NULL, "", "") != 0, "sd_register null inst");

    ASSERT(sd_deregister(NULL, "s", "i") != 0, "sd_deregister null sd");
    ASSERT(sd_deregister(sd, NULL, "i") != 0, "sd_deregister null name");
    ASSERT(sd_deregister(sd, "s", NULL) != 0, "sd_deregister null inst_id");

    ASSERT(sd_deregister_all(NULL, "s") != 0, "sd_deregister_all null sd");
    ASSERT(sd_deregister_all(sd, NULL) != 0, "sd_deregister_all null name");

    sd_instance_t buf[4];
    uint32_t found;
    ASSERT(sd_discover(NULL, "s", buf, 4, &found) != 0, "sd_discover null sd");
    ASSERT(sd_discover(sd, NULL, buf, 4, &found) != 0, "sd_discover null name");
    ASSERT(sd_discover(sd, "s", NULL, 4, &found) != 0, "sd_discover null buf");
    ASSERT(sd_discover(sd, "s", buf, 4, NULL) != 0, "sd_discover null found");

    sd_service_entry_t entries[4];
    ASSERT(sd_discover_by_type(NULL, "t", entries, 4, &found) != 0, "sd_discover_by_type null sd");
    ASSERT(sd_discover_by_type(sd, NULL, entries, 4, &found) != 0, "sd_discover_by_type null type");
    ASSERT(sd_discover_by_type(sd, "t", NULL, 4, &found) != 0, "sd_discover_by_type null entries");
    ASSERT(sd_discover_by_type(sd, "t", entries, 4, NULL) != 0, "sd_discover_by_type null found");

    ASSERT(sd_discover_by_tags(NULL, "t", entries, 4, &found) != 0, "sd_discover_by_tags null sd");
    ASSERT(sd_discover_by_tags(sd, NULL, entries, 4, &found) != 0, "sd_discover_by_tags null tags");
    ASSERT(sd_discover_by_tags(sd, "t", NULL, 4, &found) != 0, "sd_discover_by_tags null entries");
    ASSERT(sd_discover_by_tags(sd, "t", entries, 4, NULL) != 0, "sd_discover_by_tags null found");

    sd_instance_t sel;
    ASSERT(sd_select_instance(NULL, "s", SD_LB_ROUND_ROBIN, &sel) != 0,
           "sd_select_instance null sd");
    ASSERT(sd_select_instance(sd, NULL, SD_LB_ROUND_ROBIN, &sel) != 0,
           "sd_select_instance null name");
    ASSERT(sd_select_instance(sd, "s", SD_LB_ROUND_ROBIN, NULL) != 0,
           "sd_select_instance null instance");

    ASSERT(sd_heartbeat(NULL, "s", "i") != 0, "sd_heartbeat null sd");
    ASSERT(sd_heartbeat(sd, NULL, "i") != 0, "sd_heartbeat null name");
    ASSERT(sd_heartbeat(sd, "s", NULL) != 0, "sd_heartbeat null inst_id");

    ASSERT(sd_update_health(NULL, "s", "i", true) != 0, "sd_update_health null sd");
    ASSERT(sd_update_health(sd, NULL, "i", true) != 0, "sd_update_health null name");
    ASSERT(sd_update_health(sd, "s", NULL, true) != 0, "sd_update_health null inst_id");

    ASSERT(sd_update_connections(NULL, "s", "i", 10) != 0, "sd_update_connections null sd");
    ASSERT(sd_update_connections(sd, NULL, "i", 10) != 0, "sd_update_connections null name");
    ASSERT(sd_update_connections(sd, "s", NULL, 10) != 0, "sd_update_connections null inst_id");

    ASSERT(sd_get_dependencies(NULL, "s", NULL, 0) != 0, "sd_get_dependencies null sd");
    ASSERT(sd_get_dependencies(sd, NULL, NULL, 0) != 0, "sd_get_dependencies null name");

    ASSERT(sd_check_dependencies(NULL, "s", NULL, 0) != 0, "sd_check_dependencies null sd");
    ASSERT(sd_check_dependencies(sd, NULL, NULL, 0) != 0, "sd_check_dependencies null name");

    ASSERT(sd_register_event_callback(NULL, event_callback, NULL) != 0,
           "sd_register_event_callback null sd");
    ASSERT(sd_register_event_callback(sd, NULL, NULL) != 0,
           "sd_register_event_callback null callback");

    ASSERT(sd_get_stats(NULL, NULL) != 0, "sd_get_stats null sd");

    ASSERT(sd_service_count(NULL) == 0, "sd_service_count null returns 0");

    sd_destroy(sd);
    PASS();
}

/* ==================== 36. Callback on health changes ==================== */
void test_sd_callback_health_changes(void)
{
    TEST("Callback on health changes");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    callback_fired = 0;
    last_event = 0;
    sd_register_event_callback(sd, event_callback, NULL);

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    inst.healthy = true;
    sd_register(sd, "hc-svc", "test", &inst, "", "");

    int prev = callback_fired;
    sd_update_health(sd, "hc-svc", "inst-001", false);
    ASSERT(callback_fired > prev, "callback should fire on health down");
    ASSERT(last_event == SD_EVENT_INSTANCE_DOWN, "should be INSTANCE_DOWN");

    prev = callback_fired;
    sd_update_health(sd, "hc-svc", "inst-001", true);
    ASSERT(callback_fired > prev, "callback should fire on health up");
    ASSERT(last_event == SD_EVENT_INSTANCE_UP, "should be INSTANCE_UP");

    sd_destroy(sd);
    PASS();
}

/* ==================== 37. sd_deregister_all - nonexistent service ==================== */
void test_sd_deregister_all_nonexistent(void)
{
    TEST("sd_deregister_all - nonexistent service");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    int ret = sd_deregister_all(sd, "nonexistent");
    ASSERT(ret != 0, "deregister_all on nonexistent should fail");

    sd_destroy(sd);
    PASS();
}

/* ==================== 39. Multiple callbacks ==================== */
void test_sd_multiple_callbacks(void)
{
    TEST("Multiple event callbacks");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    callback_fired = 0;

    int ret = sd_register_event_callback(sd, event_callback, NULL);
    ASSERT(ret == 0, "register first callback");
    ret = sd_register_event_callback(sd, event_callback, NULL);
    ASSERT(ret == 0, "register second callback");

    sd_instance_t inst = make_instance("inst-001", "tcp://127.0.0.1:8080");
    sd_register(sd, "multi-cb-svc", "test", &inst, "", "");

    ASSERT(callback_fired >= 2, "both callbacks should be fired");

    sd_destroy(sd);
    PASS();
}

/* ==================== 40. sd_discover_by_type - null params ==================== */
void test_sd_discover_null_params(void)
{
    TEST("sd_discover - null parameter safety");
    service_discovery_t sd = sd_create(NULL);
    ASSERT(sd != NULL, "create");

    sd_instance_t buf[4];
    uint32_t found;
    ASSERT(sd_discover(NULL, "s", buf, 4, &found) != 0, "sd_discover null sd should fail");
    ASSERT(sd_discover(sd, NULL, buf, 4, &found) != 0, "sd_discover null name should fail");
    ASSERT(sd_discover(sd, "s", NULL, 4, &found) != 0, "sd_discover null instances should fail");
    ASSERT(sd_discover(sd, "s", buf, 4, NULL) != 0, "sd_discover null found_count should fail");

    sd_destroy(sd);
    PASS();
}
