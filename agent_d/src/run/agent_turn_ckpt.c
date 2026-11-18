// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_turn_ckpt.c
 * @brief 回合检查点落盘（D-12）：状态文件读写与状态迁移。
 *
 * 单一职责：`$AIRY_HOME/data/agentrt/state/agent_turn_ckpt.json` 的
 * 读改写、原子落盘（airy_io_write_file：tmp+fsync+rename）与两态迁移。
 * 与 work_hall_persist.c 同一落盘约定（state 目录、原子写），但语义不同：
 * 大厅记录任务生命周期，本文件只记录「哪个回合在第几个工具边界可续跑」。
 *
 * 全部入口以进程内互斥串行化（写侧来自工具循环线程，读侧来自 RPC 与
 * 生命周期钩子）；文件损坏按空表重建，不阻断回合推进。
 */

#include "agent_turn_ckpt.h"

#include "airy_memory.h"
#include "io.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static airy_mtx_t g_ck_lock;
static int g_ck_ready;
static int g_ck_drain; /* 退出排水：不再写、不再删，只保留 paused */

static void ck_lock(void)
{
    if (!g_ck_ready) {
        airy_mtx_init(&g_ck_lock);
        g_ck_ready = 1;
    }
    airy_mtx_lock(&g_ck_lock);
}

static void ck_unlock(void)
{
    airy_mtx_unlock(&g_ck_lock);
}

static int ck_path(char *buf, size_t n)
{
    const char *root = airy_data_dir();
    if (!root || !root[0])
        return -1;
    int w = snprintf(buf, n, "%s/agentrt/state/agent_turn_ckpt.json", root);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

static int ck_mkdir(void)
{
    const char *root = airy_data_dir();
    char dir[AIRY_PATH_MAX];
    if (!root || !root[0])
        return -1;
    int w = snprintf(dir, sizeof(dir), "%s/agentrt/state", root);
    if (w <= 0 || (size_t)w >= sizeof(dir))
        return -1;
    return airy_io_mkdir_p(dir, 0755) == 0 ? 0 : -1;
}

static cJSON *ck_load(void)
{
    char path[AIRY_PATH_MAX];
    char *buf = NULL;
    cJSON *root;
    if (ck_path(path, sizeof(path)) != 0)
        return NULL;
    if (airy_io_read_file(path, &buf, NULL) != 0)
        return NULL;
    root = cJSON_Parse(buf);
    AIRY_FREE(buf);
    if (!cJSON_IsObject(root) || !cJSON_IsArray(cJSON_GetObjectItem(root, "turns"))) {
        if (root)
            SVC_LOG_WARN("turn ckpt: state file corrupt, rebuilding");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static cJSON *ck_new(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddArrayToObject(root, "turns");
    return root;
}

static int ck_write(cJSON *root)
{
    char path[AIRY_PATH_MAX];
    char *body;
    int rc;
    if (ck_mkdir() != 0 || ck_path(path, sizeof(path)) != 0)
        return -1;
    cJSON_ReplaceItemInObject(root, "saved_at", cJSON_CreateNumber((double)time(NULL)));
    body = cJSON_PrintUnformatted(root);
    if (!body)
        return -1;
    rc = airy_io_write_file(path, body, strlen(body));
    cJSON_free(body);
    if (rc != 0)
        SVC_LOG_WARN("turn ckpt: write failed (%d)", rc);
    return rc == 0 ? 0 : -1;
}

static const char *ck_str(const cJSON *it, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(it, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int ck_same(const cJSON *it, const char *turn)
{
    const cJSON *v = cJSON_GetObjectItem(it, "turn_id");
    return cJSON_IsString(v) && strcmp(v->valuestring, turn) == 0;
}

static void ck_put_s(cJSON *it, const char *key, const char *val)
{
    cJSON_ReplaceItemInObject(it, key, cJSON_CreateString(val ? val : ""));
}

static void ck_put_n(cJSON *it, const char *key, double val)
{
    cJSON_ReplaceItemInObject(it, key, cJSON_CreateNumber(val));
}

static cJSON *ck_find(cJSON *turns, const char *turn)
{
    for (cJSON *it = turns->child; it; it = it->next) {
        if (ck_same(it, turn))
            return it;
    }
    return NULL;
}

/* turn 为 NULL 表示全表；返回迁移条目数，写盘失败返回 -1。 */
static int ck_mark(const char *turn, const char *state)
{
    cJSON *root = ck_load();
    int n = 0;
    int rc;
    if (!root)
        return 0;
    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    for (cJSON *it = turns->child; it; it = it->next) {
        if (turn && !ck_same(it, turn))
            continue;
        if (strcmp(ck_str(it, "state"), state) == 0)
            continue;
        ck_put_s(it, "state", state);
        ck_put_n(it, "updated_at", (double)time(NULL));
        n++;
    }
    rc = n > 0 ? ck_write(root) : 0;
    cJSON_Delete(root);
    return rc == 0 ? n : -1;
}

int agent_ckpt_save(const char *turn, const char *sess, int used, int limit, int tools_done,
                    const char *last_tool, const char *token)
{
    cJSON *root;
    cJSON *turns;
    cJSON *it;
    int fresh = 0;
    int rc;
    if (!turn || !turn[0] || !token || !token[0] || strlen(token) > AGENT_CKPT_TOKEN_MAX)
        return -1;
    ck_lock();
    if (g_ck_drain) {
        ck_unlock();
        return 0;
    }
    root = ck_load();
    if (!root)
        root = ck_new();
    if (!root) {
        ck_unlock();
        return -1;
    }
    turns = cJSON_GetObjectItem(root, "turns");
    it = ck_find(turns, turn);
    if (!it) {
        it = cJSON_CreateObject();
        fresh = it != NULL;
    }
    if (!it) {
        cJSON_Delete(root);
        ck_unlock();
        return -1;
    }
    ck_put_s(it, "turn_id", turn);
    ck_put_s(it, "session_id", sess);
    ck_put_s(it, "state", "running");
    ck_put_n(it, "rounds_used", used);
    ck_put_n(it, "rounds_limit", limit);
    ck_put_n(it, "tools_done", tools_done);
    ck_put_s(it, "last_tool", last_tool);
    ck_put_s(it, "resume_token", token);
    ck_put_n(it, "updated_at", (double)time(NULL));
    if (fresh)
        cJSON_InsertItemInArray(turns, 0, it);
    while (cJSON_GetArraySize(turns) > AGENT_CKPT_MAX)
        cJSON_DeleteItemFromArray(turns, cJSON_GetArraySize(turns) - 1);
    rc = ck_write(root);
    cJSON_Delete(root);
    ck_unlock();
    return rc;
}

int agent_ckpt_done(const char *turn)
{
    cJSON *root;
    cJSON *turns;
    int removed = 0;
    int rc = 0;
    if (!turn || !turn[0])
        return -1;
    ck_lock();
    if (g_ck_drain) {
        ck_unlock();
        return 0;
    }
    root = ck_load();
    if (!root) {
        ck_unlock();
        return 0;
    }
    turns = cJSON_GetObjectItem(root, "turns");
    for (int i = cJSON_GetArraySize(turns) - 1; i >= 0; i--) {
        if (ck_same(cJSON_GetArrayItem(turns, i), turn)) {
            cJSON_DeleteItemFromArray(turns, i);
            removed = 1;
        }
    }
    if (removed)
        rc = ck_write(root);
    cJSON_Delete(root);
    ck_unlock();
    return rc;
}

int agent_ckpt_pend(const char *turn)
{
    int n;
    if (!turn || !turn[0])
        return -1;
    ck_lock();
    n = ck_mark(turn, "paused");
    ck_unlock();
    return n < 0 ? -1 : 0;
}

int agent_ckpt_boot(void)
{
    int n;
    ck_lock();
    g_ck_drain = 0;
    n = ck_mark(NULL, "paused");
    ck_unlock();
    if (n > 0)
        SVC_LOG_INFO("turn ckpt: restored %d in-flight turn(s) as paused", n);
    return n < 0 ? 0 : n;
}

int agent_ckpt_drain(void)
{
    int n;
    ck_lock();
    g_ck_drain = 1;
    n = ck_mark(NULL, "paused");
    ck_unlock();
    if (n > 0)
        SVC_LOG_WARN("turn ckpt: %d in-flight turn(s) kept as paused for resume", n);
    return n < 0 ? 0 : n;
}

int agent_ckpt_count(void)
{
    cJSON *root;
    int n = 0;
    ck_lock();
    root = ck_load();
    if (root) {
        cJSON *turns = cJSON_GetObjectItem(root, "turns");
        for (cJSON *it = turns->child; it; it = it->next) {
            if (strcmp(ck_str(it, "state"), "paused") == 0)
                n++;
        }
        cJSON_Delete(root);
    }
    ck_unlock();
    return n;
}

cJSON *agent_ckpt_dump(void)
{
    cJSON *root;
    cJSON *turns;
    cJSON *out;
    cJSON *copy;
    ck_lock();
    root = ck_load();
    if (!root) {
        ck_unlock();
        out = cJSON_CreateObject();
        if (out) {
            cJSON_AddNumberToObject(out, "count", 0);
            cJSON_AddArrayToObject(out, "turns");
        }
        return out;
    }
    turns = cJSON_GetObjectItem(root, "turns");
    out = cJSON_CreateObject();
    if (out) {
        copy = cJSON_Duplicate(turns, 1);
        cJSON_AddNumberToObject(out, "count", copy ? cJSON_GetArraySize(copy) : 0);
        if (copy)
            cJSON_AddItemToObject(out, "turns", copy);
        else
            cJSON_AddArrayToObject(out, "turns");
    }
    cJSON_Delete(root);
    ck_unlock();
    return out;
}
