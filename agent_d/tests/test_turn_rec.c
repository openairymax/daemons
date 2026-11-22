// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_turn_rec.c
 * @brief 会话记忆记录构造直测（S-P0-05 读写契约）。
 *
 * 契约：一轮问答两条记录，data 为原文，metadata 带 role 枚举与 conv；
 * 空用户输入不入库，空回复只落用户一条。TUI 恢复面依赖同名字段，
 * 故字段名与取值在此逐项钉死。
 */

#include "../src/run/agent_turn_rec.c"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, name)                \
    do {                                 \
        if (cond) {                      \
            printf("PASS %s\n", (name)); \
        } else {                         \
            printf("FAIL %s\n", (name)); \
            g_fail++;                    \
        }                                \
    } while (0)

static const cJSON *meta_of(const cJSON *rec, const char *key)
{
    const cJSON *meta = cJSON_GetObjectItem(rec, "metadata");

    return meta ? cJSON_GetObjectItem(meta, key) : NULL;
}

static int str_is(const cJSON *node, const char *want)
{
    return cJSON_IsString(node) && strcmp(node->valuestring, want) == 0;
}

static void t_full_turn(void)
{
    cJSON *turn = agent_turn_rec("sess_ab12", "你好", "在的");
    const cJSON *user, *reply;

    CHECK(turn && cJSON_GetArraySize(turn) == 2, "two records");
    user = cJSON_GetArrayItem(turn, 0);
    reply = cJSON_GetArrayItem(turn, 1);
    CHECK(str_is(cJSON_GetObjectItem(user, "data"), "你好"), "user data verbatim");
    CHECK(str_is(cJSON_GetObjectItem(reply, "data"), "在的"), "reply data verbatim");
    CHECK(str_is(meta_of(user, "role"), "user"), "user role");
    CHECK(str_is(meta_of(reply, "role"), "assistant"), "reply role");
    CHECK(str_is(meta_of(user, "conv"), "sess_ab12"), "conv on user");
    CHECK(str_is(meta_of(reply, "conv"), "sess_ab12"), "conv on reply");
    CHECK(str_is(meta_of(reply, "session_id"), "sess_ab12"), "session_id kept");
    cJSON_Delete(turn);
}

static void t_edges(void)
{
    cJSON *turn = agent_turn_rec("sess_1", "only", "");
    CHECK(turn && cJSON_GetArraySize(turn) == 1, "empty reply drops record");
    cJSON_Delete(turn);

    turn = agent_turn_rec("sess_1", "only", NULL);
    CHECK(turn && cJSON_GetArraySize(turn) == 1, "null reply drops record");
    cJSON_Delete(turn);

    CHECK(agent_turn_rec("sess_1", "", "x") == NULL, "empty user rejected");
    CHECK(agent_turn_rec("sess_1", NULL, "x") == NULL, "null user rejected");

    turn = agent_turn_rec(NULL, "hi", "yo");
    CHECK(turn && str_is(meta_of(cJSON_GetArrayItem(turn, 0), "conv"), ""),
          "null conv degrades to empty");
    cJSON_Delete(turn);
}

int main(void)
{
    t_full_turn();
    t_edges();
    if (g_fail) {
        printf("FAILED %d\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
