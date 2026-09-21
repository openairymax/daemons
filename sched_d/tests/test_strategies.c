// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_strategies.c
 * @brief 调度策略枚举 ABI 值测试
 *
 * The strategy_interface_t strategy objects (src/strategies/, wired only
 * to this file) were removed as dead code; the live scoring dispatch is
 * the declarative table in sched_service_agent.c. What remains stable
 * and consumed (main.c config, sched_service_agent scoring table, DAG
 * tests) is the sched_strategy_t enum itself, so this suite keeps
 * pinning its ABI values.
 */

#include "scheduler_service.h"

#include <assert.h>
#include <stdio.h>

static void test_strategy_enum_values(void)
{
    printf("  test_strategy_enum_values...\n");

    assert(SCHED_STRATEGY_ROUND_ROBIN == 0);
    assert(SCHED_STRATEGY_WEIGHTED == 1);
    assert(SCHED_STRATEGY_ML_BASED == 2);
    assert(SCHED_STRATEGY_PRIORITY_BASED == 3);
    assert(SCHED_STRATEGY_COUNT == 4);

    assert(TASK_PRIORITY_LOW == 0);
    assert(TASK_PRIORITY_NORMAL == 1);
    assert(TASK_PRIORITY_HIGH == 2);
    assert(TASK_PRIORITY_URGENT == 3);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Scheduler Strategies Unit Tests\n");
    printf("=========================================\n");

    test_strategy_enum_values();

    printf("\nAll strategy tests PASSED\n");
    return 0;
}
