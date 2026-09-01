// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery.c
 * @brief 服务发现模块单元测试主文件（共享辅助定义与 main 入口）
 *
 * @details
 * 单元测试按功能域拆分为多个文件，本文件保留 SPDX 头、include、
 * 共享辅助（实例工厂 make_instance、事件回调 event_callback、
 * 运行计数与回调状态全局量）以及依次调用全部测试函数的 main()。
 * 各域测试函数见 test_service_discovery_lifecycle.c /
 * test_service_discovery_discover.c / test_service_discovery_select.c /
 * test_service_discovery_health.c / test_service_discovery_misc.c，
 * 共享声明见 test_service_discovery_internal.h。
 */

#include "test_service_discovery_internal.h"
#include "../include/service_discovery.h"
#include "safe_string_utils.h"
#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

int tests_run = 0;
int tests_passed = 0;

int callback_fired = 0;
sd_event_type_t last_event = 0;

void event_callback(sd_event_type_t event, const char *svc, const sd_instance_t *inst,
                    void *user_data)
{
    (void)svc;
    (void)inst;
    (void)user_data;
    callback_fired++;
    last_event = event;
}

sd_instance_t make_instance(const char *id, const char *endpoint)
{
    sd_instance_t inst;
    AIRY_MEMSET(&inst, 0, sizeof(inst));
    safe_strcpy(inst.instance_id, id, SD_MAX_NAME_LEN);
    safe_strcpy(inst.endpoint, endpoint, SD_MAX_ENDPOINT_LEN);
    inst.healthy = true;
    inst.weight = 100;
    inst.max_connections = 1000;
    return inst;
}

int main(void)
{
    printf("\n=== Service Discovery Module Unit Tests ===\n\n");

    test_sd_create_default_config();
    test_sd_create_null_config();
    test_sd_create_with_config();
    test_sd_destroy_null();
    test_sd_lifecycle();
    test_sd_start_idempotent();
    test_sd_stop_null();
    test_sd_register_normal();
    test_sd_register_null_params();
    test_sd_deregister_normal();
    test_sd_deregister_nonexistent();
    test_sd_deregister_all();
    test_sd_deregister_all_nonexistent();
    test_sd_discover_normal();
    test_sd_discover_nonexistent();
    test_sd_discover_max_count();
    test_sd_discover_only_healthy();
    test_sd_discover_null_params();
    test_sd_discover_by_type();
    test_sd_discover_by_tags();
    test_sd_select_instance_round_robin();
    test_sd_select_instance_random();
    test_sd_select_instance_weighted();
    test_sd_select_instance_least_connection();
    test_sd_select_instance_least_load();
    test_sd_select_instance_nonexistent();
    test_sd_heartbeat_normal();
    test_sd_heartbeat_nonexistent();
    test_sd_update_health();
    test_sd_callback_health_changes();
    test_sd_update_connections();
    test_sd_get_dependencies();
    test_sd_check_dependencies();
    test_sd_register_event_callback();
    test_sd_multiple_callbacks();
    test_sd_get_stats();
    test_sd_service_count();
    test_sd_lb_strategy_to_string();
    test_sd_multiple_services_stress();
    test_sd_null_parameter_safety();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
