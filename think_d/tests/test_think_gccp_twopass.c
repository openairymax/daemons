// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_think_gccp_twopass.c
 * @brief 集成测试：GCCP 两段式交互闭环（P-A, 2026-08-23）。
 *
 * 无外部 LLM（测试环境不启动 llm_d）：GCCP probe 走启发式降级，
 * need_interaction 恒为 1，第一段必然挂起并返回问题集，链路可确定性验证。
 *
 * 验证链路：
 *   1. 第一段（gccp_answers=NULL）：返回 0，结果 JSON 含
 *      gccp_need_interaction=1 与 gccp_questions 数组（非空），且不含
 *      plan（挂起，不进入后续 Phase，避免在降级目标上浪费 token）。
 *   2. 第二段（携带 gccp_answers 重发）：返回 0，结果 JSON 含 plan 对象，
 *      feedback 中 intent_confirmed 事件 data 含 "interacted":1（答案已
 *      由交互回调消费并完成目标确认）。
 *   3. 答案单次有效：第二段结束后服务端暂存答案被清理——第三次调用
 *      （不带答案）重新进入第一段语义（再次挂起），答案不泄漏到下一轮。
 *
 * @note 不依赖 llm_d 守护进程（LLM 不可用走启发式/降级路径）。
 */

#include "think_service.h"
#include "airy_memory.h"

#include <stdio.h>
#include <string.h>

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_PASS(name)                \
    do {                               \
        printf("  [PASS] %s\n", name); \
        tests_run++;                   \
        tests_passed++;                \
    } while (0)

#define TEST_FAIL(name, msg)                    \
    do {                                        \
        printf("  [FAIL] %s: %s\n", name, msg); \
        tests_run++;                            \
    } while (0)

#define CHECK(cond, name, msg)    \
    do {                          \
        if (cond) {               \
            TEST_PASS(name);      \
        } else {                  \
            TEST_FAIL(name, msg); \
        }                         \
    } while (0)

/* 在 feedback 数组中查找指定 event，并把 data 字符串拷出（无则返回 0）。 */
static int find_feedback_data(cJSON *feedback, const char *event, char *out, size_t cap)
{
    if (!feedback || !event || !out || cap == 0)
        return 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, feedback) {
        cJSON *ev = cJSON_GetObjectItem(item, "event");
        if (!cJSON_IsString(ev) || !ev->valuestring || strcmp(ev->valuestring, event) != 0)
            continue;
        cJSON *data = cJSON_GetObjectItem(item, "data");
        if (cJSON_IsString(data) && data->valuestring) {
            snprintf(out, cap, "%s", data->valuestring);
            return 1;
        }
    }
    return 0;
}

static void test_gccp_twopass(void)
{
    think_service_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    cfg.process_timeout_ms = 30000;
    cfg.max_feedback_events = 64;

    think_service_t *svc = think_service_create(&cfg);
    if (!svc) {
        TEST_FAIL("gccp_twopass", "think_service_create failed");
        return;
    }
    if (!think_service_ready(svc)) {
        TEST_FAIL("gccp_twopass", "service not ready");
        think_service_destroy(svc);
        return;
    }

    const char *prompt = "为项目实现一个带登录、检索与报表的完整系统，并保证生产级可用";

    /* ── 第一段：无答案 → 挂起并返回问题集 ── */
    think_process_result_t pass1 = {0};
    int rc = think_service_process(svc, prompt, NULL, &pass1);
    CHECK(rc == 0 && pass1.json, "pass1: rc==0 (interaction is success semantics)",
          "pass1 failed");
    if (rc == 0 && pass1.json) {
#ifdef AIRY_HAS_CJSON
        cJSON *root = cJSON_Parse(pass1.json);
        cJSON *need = root ? cJSON_GetObjectItem(root, "gccp_need_interaction") : NULL;
        CHECK(need && cJSON_IsTrue(need), "pass1: gccp_need_interaction=1",
              "need_interaction not set");
        cJSON *qarr = root ? cJSON_GetObjectItem(root, "gccp_questions") : NULL;
        if (!qarr) {
            TEST_FAIL("pass1: gccp_questions", "questions field missing");
        } else if (!cJSON_IsString(qarr) || !qarr->valuestring) {
            TEST_FAIL("pass1: gccp_questions", "questions not a JSON string");
        } else {
            cJSON *parsed = cJSON_Parse(qarr->valuestring);
            CHECK(parsed && cJSON_IsArray(parsed) && cJSON_GetArraySize(parsed) >= 1,
                  "pass1: questions array non-empty", "questions empty/invalid");
            if (parsed)
                cJSON_Delete(parsed);
        }
        cJSON *plan = root ? cJSON_GetObjectItem(root, "plan") : NULL;
        CHECK(!plan, "pass1: no plan (suspended before later phases)",
              "plan present on pending pass");
        if (root)
            cJSON_Delete(root);
#else
        CHECK(strstr(pass1.json, "\"gccp_need_interaction\":1") != NULL,
              "pass1: gccp_need_interaction=1", "field missing");
        CHECK(strstr(pass1.json, "\"gccp_questions\"") != NULL,
              "pass1: gccp_questions present", "field missing");
        CHECK(strstr(pass1.json, "\"plan\"") == NULL, "pass1: no plan", "plan present");
#endif
    }

    /* ── 第二段：携带答案重发 → 目标确认并生成 plan，interacted=1 ── */
    const char *answers = "{\"endpoint\":\"交付可运行的完整系统与测试报告\","
                          "\"start\":\"已有 agentrt 底座与 llm 服务\","
                          "\"bottleneck\":\"需补齐登录与报表模块并做生产加固\","
                          "\"audience\":\"平台运维团队按验收标准验收\"}";
    think_process_result_t pass2 = {0};
    rc = think_service_process(svc, prompt, answers, &pass2);
    CHECK(rc == 0 && pass2.json, "pass2: rc==0 (answers consumed)", "pass2 failed");
    if (rc == 0 && pass2.json) {
#ifdef AIRY_HAS_CJSON
        cJSON *root = cJSON_Parse(pass2.json);
        cJSON *plan = root ? cJSON_GetObjectItem(root, "plan") : NULL;
        if (!plan || !cJSON_IsObject(plan)) {
            TEST_FAIL("pass2: plan generated", "plan missing");
        } else {
            cJSON *nodes = cJSON_GetObjectItem(plan, "nodes");
            CHECK(nodes && cJSON_IsArray(nodes) && cJSON_GetArraySize(nodes) >= 1,
                  "pass2: plan nodes non-empty", "plan nodes empty");
        }
        cJSON *fb = root ? cJSON_GetObjectItem(root, "feedback") : NULL;
        char data[512] = "";
        if (!fb || !find_feedback_data(fb, "intent_confirmed", data, sizeof(data))) {
            TEST_FAIL("pass2: intent_confirmed feedback", "event missing");
        } else {
            CHECK(strstr(data, "\"interacted\":1") != NULL,
                  "pass2: intent_confirmed interacted=1", "interacted != 1");
        }
        if (root)
            cJSON_Delete(root);
#else
        CHECK(strstr(pass2.json, "\"plan\"") != NULL, "pass2: plan generated",
              "plan missing");
        CHECK(strstr(pass2.json, "\"intent_confirmed\"") != NULL,
              "pass2: intent_confirmed feedback", "event missing");
        CHECK(strstr(pass2.json, "\"interacted\":1") != NULL,
              "pass2: interacted=1", "interacted != 1");
#endif
    }

    /* ── 第三段：答案单次有效，再次不带答案 → 重新挂起（不泄漏） ── */
    think_process_result_t pass3 = {0};
    rc = think_service_process(svc, prompt, NULL, &pass3);
    CHECK(rc == 0 && pass3.json, "pass3: rc==0 (answers not leaked)", "pass3 failed");
    if (rc == 0 && pass3.json) {
#ifdef AIRY_HAS_CJSON
        cJSON *root = cJSON_Parse(pass3.json);
        cJSON *need = root ? cJSON_GetObjectItem(root, "gccp_need_interaction") : NULL;
        CHECK(need && cJSON_IsTrue(need), "pass3: re-pending (answers cleared)",
              "answers leaked into pass3");
        if (root)
            cJSON_Delete(root);
#else
        CHECK(strstr(pass3.json, "\"gccp_need_interaction\":1") != NULL,
              "pass3: re-pending (answers cleared)", "answers leaked");
#endif
    }

    think_result_free(&pass1);
    think_result_free(&pass2);
    think_result_free(&pass3);
    think_service_destroy(svc);
}

int main(void)
{
    printf("[SUITE] test_think_gccp_twopass\n");
    test_gccp_twopass();

    printf("\n[RESULT] %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
