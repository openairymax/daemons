// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_prov_stat.c
 * @brief 0.1.20 D2-9：传输观测件回归——三类超时分类计数、时延环形采样与
 *        p99、自动预算的 floor/无样本缺省/p99*1.5 三态、复位与槽位耗尽。
 */

#include <stdio.h>

#include "platform_base.h"
#include "providers/core/prov_stat.h"

static int g_fail;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

#define EPS 0.001

static void test_tmo_counters(void)
{
    prov_stat_reset();
    EXPECT(prov_stat_get(PROV_TMO_CONNECT) == 0U);

    prov_stat_hit(PROV_TMO_CONNECT);
    prov_stat_hit(PROV_TMO_CONNECT);
    prov_stat_hit(PROV_TMO_STALL);
    prov_stat_hit(PROV_TMO_TOTAL);

    EXPECT(prov_stat_get(PROV_TMO_CONNECT) == 2U);
    EXPECT(prov_stat_get(PROV_TMO_STALL) == 1U);
    EXPECT(prov_stat_get(PROV_TMO_TOTAL) == 1U);

    /* 越界类别是调用方的契约错误：读写都必须退化为无操作/零，不得越界。 */
    prov_stat_hit((prov_tmo_t)PROV_TMO_KIND_N);
    prov_stat_hit((prov_tmo_t)-1);
    EXPECT(prov_stat_get((prov_tmo_t)PROV_TMO_KIND_N) == 0U);
    EXPECT(prov_stat_get((prov_tmo_t)-1) == 0U);
}

static void test_lat_empty_and_default(void)
{
    prov_stat_reset();
    EXPECT(prov_stat_p99("nobody") < EPS);
    /* 无样本不足以推导预算，回落保守缺省 180s。 */
    EXPECT(prov_tmo_auto("nobody") > 180.0 - EPS && prov_tmo_auto("nobody") < 180.0 + EPS);
}

static void test_lat_p99_and_floor(void)
{
    prov_stat_reset();
    for (uint32_t i = 1U; i <= PROV_STAT_SLOTS; i++)
        prov_stat_lat("fast", i);

    /* 满槽时 p99 即最大值（idx=(n*99+99)/100-1）。 */
    EXPECT(prov_stat_p99("fast") > 64.0 - EPS && prov_stat_p99("fast") < 64.0 + EPS);
    /* p99*1.5=96s 低于下限，预算被 floor 抬到 120s。 */
    EXPECT(prov_tmo_auto("fast") > 120.0 - EPS && prov_tmo_auto("fast") < 120.0 + EPS);
}

static void test_lat_scale_above_floor(void)
{
    prov_stat_reset();
    for (uint32_t i = 0U; i < PROV_STAT_SLOTS; i++)
        prov_stat_lat("slow", 200U);

    EXPECT(prov_stat_p99("slow") > 200.0 - EPS && prov_stat_p99("slow") < 200.0 + EPS);
    /* 200s 已在 floor 之上，预算取 p99*1.5。 */
    EXPECT(prov_tmo_auto("slow") > 300.0 - EPS && prov_tmo_auto("slow") < 300.0 + EPS);
}

static void test_lat_ring_wraparound(void)
{
    prov_stat_reset();
    for (uint32_t i = 1U; i <= 100U; i++)
        prov_stat_lat("ring", i);

    /* 环形缓冲只保留最近 PROV_STAT_SLOTS 笔（37..100），p99 取 100。 */
    EXPECT(prov_stat_p99("ring") > 100.0 - EPS && prov_stat_p99("ring") < 100.0 + EPS);
}

static void test_group_exhaustion(void)
{
    static const char *names[PROV_STAT_GROUPS + 1] = {"g0", "g1", "g2", "g3",
                                                      "g4", "g5", "g6", "g7", "g8"};
    prov_stat_reset();
    for (int i = 0; i <= PROV_STAT_GROUPS; i++) {
        for (uint32_t k = 0U; k < PROV_STAT_SLOTS; k++)
            prov_stat_lat(names[i], 50U);
    }

    /* 前 8 组正常入样，第 9 组无槽位：采样静默丢弃，不得踩踏既有组。 */
    EXPECT(prov_stat_p99("g0") > 50.0 - EPS && prov_stat_p99("g0") < 50.0 + EPS);
    EXPECT(prov_stat_p99("g7") > 50.0 - EPS && prov_stat_p99("g7") < 50.0 + EPS);
    EXPECT(prov_stat_p99("g8") < EPS);
}

static void test_reset_clears_all(void)
{
    prov_stat_reset();
    prov_stat_hit(PROV_TMO_TOTAL);
    prov_stat_lat("z", 500U);
    EXPECT(prov_stat_get(PROV_TMO_TOTAL) == 1U);
    EXPECT(prov_stat_p99("z") > 500.0 - EPS);

    prov_stat_reset();
    EXPECT(prov_stat_get(PROV_TMO_TOTAL) == 0U);
    EXPECT(prov_stat_p99("z") < EPS);
    EXPECT(prov_tmo_auto("z") > 180.0 - EPS && prov_tmo_auto("z") < 180.0 + EPS);
}

int main(void)
{
    printf("=========================================\n");
    printf("  prov_stat: transport observation\n");
    printf("=========================================\n");

    test_tmo_counters();
    test_lat_empty_and_default();
    test_lat_p99_and_floor();
    test_lat_scale_above_floor();
    test_lat_ring_wraparound();
    test_group_exhaustion();
    test_reset_clears_all();

    if (g_fail == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_fail);
    return 1;
}
