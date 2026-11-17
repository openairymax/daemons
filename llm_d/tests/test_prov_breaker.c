// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_prov_breaker.c
 * @brief 0.1.21 B-4 / C-3：provider 层熔断回归——第 N+1 次必然熔断、
 *        超时族与预算口径分离、取消不计、半开恢复、闸门关闭与读面。
 */

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "airy_memory.h"
#include "error.h"
#include "platform_misc.h"

#include "airy_prov/prov_breaker.h"

static int g_fail;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static void setup(uint32_t fail_max, uint32_t open_ms)
{
    prov_brk_cfg_t c = {fail_max, open_ms, 1};
    EXPECT(prov_brk_cfg(&c) == 1);
}

/* 读面与缺省：未记账的 provider 不因观察而建账，空身份一律放行。 */
static void test_read_faces(void)
{
    prov_brk_reset();

    prov_brk_stat_t st;
    EXPECT(prov_brk_state("nobody") == PROV_BRK_NONE);
    EXPECT(prov_brk_stat("nobody", &st) == -1);
    EXPECT(prov_brk_stat("nobody", NULL) == -1);
    EXPECT(prov_brk_allow(NULL) == 1);
    EXPECT(prov_brk_allow("") == 1);
    EXPECT(strcmp(prov_brk_text(PROV_BRK_CLOSED), "closed") == 0);
    EXPECT(strcmp(prov_brk_text(PROV_BRK_OPEN), "open") == 0);
    EXPECT(strcmp(prov_brk_text(PROV_BRK_HALF), "half_open") == 0);
    EXPECT(strcmp(prov_brk_text(PROV_BRK_NONE), "none") == 0);
    EXPECT(prov_brk_breaks() == 0U);
}

/* C-3 判据：连续失败到阈值后，第 N+1 次必然熔断（不再放行）。 */
static void test_open_on_threshold(void)
{
    setup(3, 30000);
    prov_brk_reset();

    for (int i = 0; i < 3; i++) {
        EXPECT(prov_brk_allow("p1") == 1);
        prov_brk_fail("p1", AIRY_ERR_UNKNOWN);
    }
    EXPECT(prov_brk_state("p1") == PROV_BRK_OPEN);

    /* 第 4 次：闸门拒绝，且拒绝不增加放行列。 */
    EXPECT(prov_brk_allow("p1") == 0);
    EXPECT(prov_brk_allow("p1") == 0);

    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p1", &st) == 0);
    EXPECT(st.calls == 3U);
    EXPECT(st.fails == 3U);
    EXPECT(st.rejects == 2U);
    EXPECT(st.breaks == 2U);
    EXPECT(st.opens == 1U);
    EXPECT(st.state == PROV_BRK_OPEN);
    EXPECT(prov_brk_breaks() == 2U);

    char *js = prov_brk_json();
    EXPECT(js != NULL);
    cJSON *arr = js ? cJSON_Parse(js) : NULL;
    AIRY_FREE(js);
    EXPECT(arr != NULL);
    if (arr) {
        EXPECT(cJSON_GetArraySize(arr) == 1);
        cJSON *o = cJSON_GetArrayItem(arr, 0);
        cJSON *name = cJSON_GetObjectItem(o, "provider");
        cJSON *state = cJSON_GetObjectItem(o, "state");
        cJSON *breaks = cJSON_GetObjectItem(o, "breaks");
        EXPECT(name && strcmp(name->valuestring, "p1") == 0);
        EXPECT(state && strcmp(state->valuestring, "open") == 0);
        EXPECT(breaks && breaks->valuedouble == 2.0);
        cJSON_Delete(arr);
    }
}

/* 超时族（连接/停摆/整轮传输预算）一律按"端点慢"计入连败。 */
static void test_timeout_family(void)
{
    setup(2, 30000);
    prov_brk_reset();

    EXPECT(prov_brk_allow("p2") == 1);
    prov_brk_fail("p2", AIRY_ETIMEDOUT);
    EXPECT(prov_brk_allow("p2") == 1);
    prov_brk_fail("p2", AIRY_ERR_LLM_STREAM_STALL);

    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p2", &st) == 0);
    EXPECT(st.slow_tmo == 2U);
    EXPECT(st.fails == 0U);
    EXPECT(st.state == PROV_BRK_OPEN);
}

/* 链级预算耗尽不是端点故障：只记预算口径，不推进连败、不跳闸。 */
static void test_budget_not_trip(void)
{
    setup(2, 30000);
    prov_brk_reset();

    for (int i = 0; i < 4; i++) {
        EXPECT(prov_brk_allow("p3") == 1);
        prov_brk_tmo("p3", PROV_TMO_TOTAL);
    }

    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p3", &st) == 0);
    EXPECT(st.budget_tmo == 4U);
    EXPECT(st.slow_tmo == 0U);
    EXPECT(st.calls == 4U);
    /* 单口径：预算终止与熔断拒绝同记 breaks，但仍不推进连败、不打开。 */
    EXPECT(st.breaks == 4U);
    EXPECT(st.rejects == 0U);
    EXPECT(st.opens == 0U);
    EXPECT(st.state == PROV_BRK_CLOSED);
}

/* 下游取消不是端点故障：不记失败、不推进连败。 */
static void test_cancel_ignored(void)
{
    setup(1, 30000);
    prov_brk_reset();

    EXPECT(prov_brk_allow("p4") == 1);
    prov_brk_fail("p4", AIRY_ERR_CANCELED);

    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p4", &st) == 0);
    EXPECT(st.fails == 0U);
    EXPECT(st.slow_tmo == 0U);
    EXPECT(st.state == PROV_BRK_CLOSED);
}

/* 冷却后进入半开：放行一次探测，成功即闭合，不再累计打开次数。 */
static void test_half_open_recover(void)
{
    setup(1, 10);
    prov_brk_reset();

    EXPECT(prov_brk_allow("p5") == 1);
    prov_brk_fail("p5", AIRY_ERR_UNKNOWN);
    EXPECT(prov_brk_state("p5") == PROV_BRK_OPEN);

    airy_sleep_ms(30);
    EXPECT(prov_brk_allow("p5") == 1);
    EXPECT(prov_brk_state("p5") == PROV_BRK_HALF);

    prov_brk_ok("p5");
    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p5", &st) == 0);
    EXPECT(st.state == PROV_BRK_CLOSED);
    EXPECT(st.opens == 1U);
    EXPECT(st.oks == 1U);
}

/* fail_max=0 是关闭闸门：计数照记，但永不跳闸、不污染 opens。 */
static void test_gate_off(void)
{
    setup(0, 30000);
    prov_brk_reset();

    for (int i = 0; i < 10; i++) {
        EXPECT(prov_brk_allow("p6") == 1);
        prov_brk_fail("p6", AIRY_ERR_UNKNOWN);
    }

    prov_brk_stat_t st;
    EXPECT(prov_brk_stat("p6", &st) == 0);
    EXPECT(st.calls == 10U);
    EXPECT(st.fails == 10U);
    EXPECT(st.rejects == 0U);
    EXPECT(st.opens == 0U);
    EXPECT(st.state == PROV_BRK_CLOSED);
}

int main(void)
{
    printf("=========================================\n");
    printf("  prov_breaker: provider circuit breaker\n");
    printf("=========================================\n");

    test_read_faces();
    test_open_on_threshold();
    test_timeout_family();
    test_budget_not_trip();
    test_cancel_ignored();
    test_half_open_recover();
    test_gate_off();

    if (g_fail == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_fail);
    return 1;
}
