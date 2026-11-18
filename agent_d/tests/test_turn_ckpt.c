// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_turn_ckpt.c
 * @brief 回合检查点（D-12）白盒直测：两态迁移、上限裁剪、损坏重建。
 *
 * 只验证落盘契约本身，不起 daemon：save/pend/done 的行为即
 * 「重启后能否取回令牌」的全部依据。
 */

#include "../src/run/agent_turn_ckpt.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static cJSON *find_turn(const char *turn)
{
    cJSON *root = agent_ckpt_dump();
    if (!root)
        return NULL;
    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    cJSON *hit = NULL;
    for (int i = 0; turns && i < cJSON_GetArraySize(turns); i++) {
        cJSON *it = cJSON_GetArrayItem(turns, i);
        cJSON *id = cJSON_GetObjectItem(it, "turn_id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, turn) == 0) {
            hit = cJSON_Duplicate(it, 1);
            break;
        }
    }
    cJSON_Delete(root);
    return hit;
}

static int turn_state(const char *turn, const char *state)
{
    cJSON *it = find_turn(turn);
    if (!it)
        return 0;
    cJSON *st = cJSON_GetObjectItem(it, "state");
    int ok = cJSON_IsString(st) && strcmp(st->valuestring, state) == 0;
    cJSON_Delete(it);
    return ok;
}

static int turn_token(const char *turn, const char *token)
{
    cJSON *it = find_turn(turn);
    if (!it)
        return 0;
    cJSON *t = cJSON_GetObjectItem(it, "resume_token");
    int ok = cJSON_IsString(t) && strcmp(t->valuestring, token) == 0;
    cJSON_Delete(it);
    return ok;
}

static int turns_len(void)
{
    cJSON *root = agent_ckpt_dump();
    if (!root)
        return -1;
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(root, "turns"));
    cJSON_Delete(root);
    return n;
}

static void test_lifecycle(void)
{
    CHECK(agent_ckpt_boot() == 0 && agent_ckpt_count() == 0, "boot: empty store");

    CHECK(agent_ckpt_save("t1", "s1", 2, 8, 2, "fs_read", "tok-1") == 0, "save: ok");
    CHECK(agent_ckpt_count() == 0, "save: running is not resumable");
    CHECK(turn_state("t1", "running"), "save: entry is running");

    CHECK(agent_ckpt_pend("t1") == 0 && agent_ckpt_count() == 1, "pend: resumable");
    CHECK(turn_state("t1", "paused"), "pend: running -> paused");

    /* 重启恢复：上次遗留一律视为可续跑。 */
    CHECK(agent_ckpt_boot() == 0 && agent_ckpt_count() == 1, "boot: paused kept");

    CHECK(agent_ckpt_done("t1") == 0, "done: terminal removes entry");
    CHECK(agent_ckpt_count() == 0 && turns_len() == 0, "done: store empty");

    CHECK(agent_ckpt_save("t2", "s2", 1, 4, 1, "shell_run", "") < 0,
          "save: empty token rejected");
    CHECK(agent_ckpt_save("t2", "s2", 1, 4, 1, "shell_run", "tok-2") == 0, "save: t2");
    agent_ckpt_done("t2");
}

static void test_drain(void)
{
    CHECK(agent_ckpt_save("d1", "s", 1, 8, 1, "fs_read", "tok-d1") == 0, "drain: seed");
    CHECK(agent_ckpt_drain() == 1, "drain: running -> paused");
    CHECK(turn_state("d1", "paused"), "drain: entry paused");

    /* 排水期不覆盖最后令牌、不删除条目。 */
    CHECK(agent_ckpt_save("d1", "s", 2, 8, 2, "fs_write", "tok-late") == 0,
          "drain: save suppressed");
    CHECK(turn_token("d1", "tok-d1"), "drain: last token preserved");
    agent_ckpt_done("d1");
    CHECK(turn_state("d1", "paused"), "drain: done keeps paused entry");

    /* 重启后排水标记解除，新回合可正常落盘。 */
    agent_ckpt_boot();
    CHECK(agent_ckpt_save("d2", "s", 1, 8, 1, "fs_read", "tok-d2") == 0,
          "boot: drain flag cleared");
    CHECK(turn_state("d2", "running"), "boot: write resumed");
    agent_ckpt_done("d1");
    agent_ckpt_done("d2");
}

static void test_cap(void)
{
    for (int i = 0; i < AGENT_CKPT_MAX + 3; i++) {
        char turn[16], tok[16];
        snprintf(turn, sizeof(turn), "c%d", i);
        snprintf(tok, sizeof(tok), "tok-%d", i);
        agent_ckpt_save(turn, "s", i, 8, i, "fs_read", tok);
    }
    CHECK(turns_len() == AGENT_CKPT_MAX, "cap: oldest entries trimmed");
    CHECK(turn_state("c10", "running") && !turn_state("c0", "running"),
          "cap: newest kept, oldest dropped");
    for (int i = 0; i < AGENT_CKPT_MAX + 3; i++) {
        char turn[16];
        snprintf(turn, sizeof(turn), "c%d", i);
        agent_ckpt_done(turn);
    }
    CHECK(turns_len() == 0, "cap: cleanup");
}

static void test_corrupt(const char *root)
{
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/agentrt/state", root);
    snprintf(path, sizeof(path), "%s/agent_turn_ckpt.json", dir);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fputs("{not-json", f);
    fclose(f);

    CHECK(agent_ckpt_boot() == 0, "corrupt: boot tolerates damage");
    CHECK(agent_ckpt_save("x1", "s", 1, 8, 1, "fs_read", "tok-x") == 0,
          "corrupt: rebuilt on next save");
    CHECK(turns_len() == 1, "corrupt: store rebuilt");
    agent_ckpt_done("x1");
}

int main(void)
{
    char home[128];
    snprintf(home, sizeof(home), "/tmp/agent_d_turn_ckpt_%ld", (long)getpid());
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
    if (system(cmd) != 0)
        return 1;
    setenv("AIRY_HOME", home, 1);

    test_lifecycle();
    test_drain();
    test_cap();
    test_corrupt(airy_data_dir());

    printf(g_fail ? "FAILED %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
