// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_ledger.c
 * @brief 台账接线纯函数直测（white-box include，无 mem_d 依赖）。
 */

#include "../src/agent_run_ledger.c"

#include <stdio.h>

static int g_fail = 0;

#define CHECK(cond, name)                \
    do {                                 \
        if (cond) {                      \
            printf("PASS %s\n", (name)); \
        } else {                         \
            printf("FAIL %s\n", (name)); \
            g_fail++;                    \
        }                                \
    } while (0)

static cJSON *mk_msg(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

static void test_init(void)
{
    agent_ledger_t lg;
    agent_ledger_init(&lg, NULL);
    CHECK(lg.enabled == 0, "init: NULL sess disabled");
    agent_ledger_init(&lg, "");
    CHECK(lg.enabled == 0, "init: empty sess disabled");
    agent_ledger_init(&lg, "sess_abc");
    CHECK(lg.enabled == 1 && strstr(lg.sock, "mem.sock") != NULL,
          "init: valid sess enabled with mem.sock");
}

static void test_item_push(void)
{
    agent_ledger_t lg;
    AIRY_MEMSET(&lg, 0, sizeof(lg));
    cJSON *m = mk_msg("user", "hi");
    for (int i = 0; i < 20; i++) {
        char id[33];
        snprintf(id, sizeof(id), "%032x", i);
        lg_item_push(&lg, id, "user", m);
    }
    CHECK(lg.count == 20 && lg.cap >= 20, "item_push: amortized growth to 20");
    CHECK(strcmp(lg.items[19].entry_id, "00000000000000000000000000000013") == 0,
          "item_push: entry_id stored");
    agent_ledger_free(&lg);
    CHECK(lg.items == NULL && lg.count == 0, "free: map cleared");
    cJSON_Delete(m);
}

static void test_protect_start(void)
{
    agent_ledger_t lg;
    AIRY_MEMSET(&lg, 0, sizeof(lg));
    CHECK(lg_protect_start(&lg) == 0, "protect_start: empty map");

    lg_item_push(&lg, "a", "user", NULL);
    lg_item_push(&lg, "b", "tool_result", NULL);
    lg_item_push(&lg, "c", "assistant", NULL);
    lg_item_push(&lg, "d", "tool_result", NULL);
    CHECK(lg_protect_start(&lg) == 2, "protect_start: last assistant index");
    agent_ledger_free(&lg);
}

static void test_cand_entries(void)
{
    agent_ledger_t lg;
    AIRY_MEMSET(&lg, 0, sizeof(lg));
    cJSON *messages = cJSON_CreateArray();
    cJSON *u0 = mk_msg("user", "entry");
    cJSON *u1 = mk_msg("user", "hello");
    cJSON *a1 = mk_msg("assistant", "final");
    cJSON_AddItemToArray(messages, u0);
    cJSON_AddItemToArray(messages, u1);
    cJSON_AddItemToArray(messages, a1);

    lg_item_push(&lg, "id0", "user", u0);
    lg_item_push(&lg, "", "user", NULL); /* 已压缩占位：跳过 */
    lg_item_push(&lg, "id2", "user", u1);
    lg_item_push(&lg, "id3", "tool_result", NULL); /* 消息失联：跳过 */
    lg_item_push(&lg, "id4", "assistant", a1);     /* 受保护：不候选 */

    cJSON *arr = lg_cand_entries(&lg, 4);
    CHECK(arr != NULL && cJSON_GetArraySize(arr) == 1, "cand_entries: only id2");
    cJSON *eid = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "entry_id");
    cJSON *etxt = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "text");
    cJSON *ety = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "entry_type");
    CHECK(eid && strcmp(eid->valuestring, "id2") == 0, "cand_entries: entry_id");
    CHECK(etxt && strcmp(etxt->valuestring, "hello") == 0, "cand_entries: text from msg");
    CHECK(ety && strcmp(ety->valuestring, "user") == 0, "cand_entries: entry_type");
    cJSON_Delete(arr);
    agent_ledger_free(&lg);
    cJSON_Delete(messages);
}

static void test_apply(void)
{
    agent_ledger_t lg;
    AIRY_MEMSET(&lg, 0, sizeof(lg));
    cJSON *messages = cJSON_CreateArray();
    cJSON *u0 = mk_msg("user", "entry");
    cJSON *u1 = mk_msg("user", "hello");
    cJSON *t1 = mk_msg("tool", "res1");
    cJSON *a1 = mk_msg("assistant", "final");
    cJSON_AddItemToArray(messages, u0);
    cJSON_AddItemToArray(messages, u1);
    cJSON_AddItemToArray(messages, t1);
    cJSON_AddItemToArray(messages, a1);

    lg_item_push(&lg, "id0", "user", u0);
    lg_item_push(&lg, "id1", "user", u1);
    lg_item_push(&lg, "id2", "tool_result", t1);
    lg_item_push(&lg, "id3", "assistant", a1);

    size_t protect = lg_protect_start(&lg);
    CHECK(protect == 3, "apply: protect at last assistant");
    lg_apply(&lg, messages, protect, "SUMMARY", 2);

    CHECK(cJSON_GetArraySize(messages) == 3, "apply: candidates replaced by summary");
    cJSON *s1 = cJSON_GetArrayItem(messages, 1);
    cJSON *srole = cJSON_GetObjectItem(s1, "role");
    cJSON *scont = cJSON_GetObjectItem(s1, "content");
    CHECK(srole && strcmp(srole->valuestring, "user") == 0 && scont &&
              strcmp(scont->valuestring, "[compressed context]\nSUMMARY") == 0,
          "apply: summary inserted in place");
    CHECK(cJSON_GetArrayItem(messages, 2) == a1, "apply: protected tail intact");

    CHECK(lg.count == 3, "apply: map rebuilt");
    /* 重建顺序：入口 + 受保护项 + 摘要占位（占位 entry_id 空，永不候选）。 */
    CHECK(lg.items[0].msg == u0 && lg.items[1].msg == a1, "apply: map anchors intact");
    CHECK(lg.items[2].entry_id[0] == '\0' && lg.items[2].msg == s1,
          "apply: placeholder maps to summary");
    int dangling = 0;
    for (size_t i = 0; i < lg.count; ++i) {
        if (!lg.items[i].msg)
            continue;
        int found = 0;
        for (int j = 0; j < cJSON_GetArraySize(messages); ++j)
            if (cJSON_GetArrayItem(messages, j) == lg.items[i].msg)
                found = 1;
        if (!found)
            dangling = 1;
    }
    CHECK(!dangling, "apply: no dangling msg pointers");

    agent_ledger_free(&lg);
    cJSON_Delete(messages);
}

int main(void)
{
    test_init();
    test_item_push();
    test_protect_start();
    test_cand_entries();
    test_apply();
    if (g_fail) {
        printf("FAILED: %d\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
