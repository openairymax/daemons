// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_ledger.c
 * @brief Agent run 上下文台账接线（记账 + 预算 + 压缩闭环）。
 *
 * 实现 13-semantic-cache-context-ledger.md 的调用方语义：
 *  - 记账：入口 user / 每轮 assistant（显式 token_out）/ 每个工具结果，
 *    逐条 mem.ledger_append；text 驱动 token_standard 估算；以返回的
 *    ledger_id（批次首条 entry_id）建立 entry_id ↔ 消息指针映射。
 *  - 适配：每轮 LLM 调用前 mem.ledger_window 预算校验；warn 时按 ReAct
 *    轮次成组（assistant(tool_calls) + tool 原子组，防孤儿 tool 消息）
 *    提交 mem.compress，用返回的压缩上下文替换本地消息组；mem_d 侧
 *    自动完成 ledger.mark 与压缩块追加。
 *  - 降级：sess 为空或 mem_d 不可达时静默跳过，任何台账/压缩失败不阻断
 *    主对话（渐进式降级）。
 *
 * 压缩作用域为本 run 的 ReAct 循环（工具结果膨胀主场景）；跨 run 历史
 * 的窗口装配由客户端 history 演进，见 0.1.17 收敛清单 W-2 v2。
 */

#include "agent_run_internal.h"

#include "airy_memory.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

#define LG_TIMEOUT_MS 5000
#define LG_PREFIX "[compressed context]\n"

int agent_ledger_init(agent_ledger_t *lg, const char *sess)
{
    AIRY_MEMSET(lg, 0, sizeof(*lg));
    if (!sess || !sess[0] || strlen(sess) >= sizeof(lg->sess))
        return 0;
    AIRY_STRNCPY_TERM(lg->sess, sess, sizeof(lg->sess));
    snprintf(lg->sock, sizeof(lg->sock), "%s", airy_runtime_dir_socket("mem.sock"));
    if (!lg->sock[0])
        return 0;
    lg->enabled = 1;
    return 0;
}

void agent_ledger_free(agent_ledger_t *lg)
{
    if (lg->items)
        AIRY_FREE(lg->items);
    lg->items = NULL;
    lg->count = lg->cap = 0;
}

/* mem_d RPC 泛型：params 对象序列化后调用，返回堆上 result JSON。 */
static char *lg_call(const agent_ledger_t *lg, const char *method, cJSON *params)
{
    char *params_str = cJSON_PrintUnformatted(params);
    if (!params_str)
        return NULL;
    char *resp = NULL;
    int rc = daemon_rpc_call(lg->sock, method, params_str, &resp, LG_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS)
        return NULL;
    return resp;
}

/* 映射追加（amortized O(1)；分配失败仅丢映射，不影响记账）。 */
static void lg_item_push(agent_ledger_t *lg, const char *entry_id, const char *type, cJSON *msg)
{
    if (lg->count == lg->cap) {
        size_t ncap = lg->cap ? lg->cap * 2 : 8;
        agent_ledger_item_t *ni =
            (agent_ledger_item_t *)AIRY_REALLOC(lg->items, ncap * sizeof(agent_ledger_item_t));
        if (!ni)
            return;
        lg->items = ni;
        lg->cap = ncap;
    }
    agent_ledger_item_t *it = &lg->items[lg->count++];
    AIRY_STRNCPY_TERM(it->entry_id, entry_id, sizeof(it->entry_id));
    AIRY_STRNCPY_TERM(it->type, type, sizeof(it->type));
    it->msg = msg;
}

void agent_ledger_add(agent_ledger_t *lg, const char *type, const char *text, size_t token_out,
                      cJSON *msg)
{
    if (!lg->enabled || !type || !msg)
        return;
    cJSON *params = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    cJSON *e = cJSON_CreateObject();
    if (!params || !entries || !e) {
        cJSON_Delete(params);
        cJSON_Delete(entries);
        cJSON_Delete(e);
        return;
    }
    cJSON_AddStringToObject(params, "session_id", lg->sess);
    cJSON_AddStringToObject(e, "entry_type", type);
    if (text)
        cJSON_AddStringToObject(e, "text", text);
    if (token_out > 0)
        cJSON_AddNumberToObject(e, "token_out", (double)token_out);
    cJSON_AddStringToObject(e, "source", "agent");
    cJSON_AddItemToArray(entries, e);
    cJSON_AddItemToObject(params, "entries", entries);

    char *resp = lg_call(lg, "ledger_append", params);
    cJSON_Delete(params);
    if (!resp) {
        SVC_LOG_DEBUG("agent.run: ledger_append failed (session=%s)", lg->sess);
        return;
    }
    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    cJSON *lid = root ? cJSON_GetObjectItem(root, "ledger_id") : NULL;
    lg_item_push(lg, cJSON_IsString(lid) ? lid->valuestring : "", type, msg);
    cJSON_Delete(root);
}

/* 候选起点：最后一个 assistant 映射项（其后的 tool 链与正在构造的下一
 * 轮请求受保护）；无 assistant 时返回 count（无候选）。 */
static size_t lg_protect_start(const agent_ledger_t *lg)
{
    for (size_t i = lg->count; i > 0; --i) {
        if (strcmp(lg->items[i - 1].type, "assistant") == 0)
            return i - 1;
    }
    return lg->count;
}

/* 压缩候选条目 [1, protect)：入口 user 保留原文；entry_id 为空（压缩
 * 过的摘要块）或消息已失联的条目不再候选。返回 NULL 表示无候选。 */
static cJSON *lg_cand_entries(const agent_ledger_t *lg, size_t protect)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    for (size_t i = 1; i < protect; ++i) {
        const agent_ledger_item_t *it = &lg->items[i];
        if (!it->entry_id[0] || !it->msg)
            continue;
        cJSON *content = cJSON_GetObjectItem(it->msg, "content");
        if (!cJSON_IsString(content))
            continue;
        cJSON *e = cJSON_CreateObject();
        if (!e)
            continue;
        cJSON_AddStringToObject(e, "entry_id", it->entry_id);
        cJSON_AddStringToObject(e, "entry_type", it->type);
        cJSON_AddStringToObject(e, "text", content->valuestring);
        cJSON_AddItemToArray(arr, e);
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

/* 判断消息是否属于候选映射区 [1, protect)。 */
static int lg_is_candidate(const agent_ledger_t *lg, size_t protect, const cJSON *msg)
{
    for (size_t j = 1; j < protect; ++j) {
        if (lg->items[j].msg == msg)
            return 1;
    }
    return 0;
}

/* 本地替换：删除候选映射的消息，原位插入压缩摘要 user 消息；映射重建
 * 保证任何路径下不残留悬空 msg 指针。n_entries 仅为日志条数（entries
 * 所有权已随 compress RPC params 释放，此处不得再引用）。 */
static void lg_apply(agent_ledger_t *lg, cJSON *messages, size_t protect, const char *context,
                     int n_entries)
{
    cJSON *summary = cJSON_CreateObject();
    size_t text_len = strlen(LG_PREFIX) + strlen(context) + 1;
    char *body = (char *)AIRY_MALLOC(text_len);
    if (!summary || !body) {
        cJSON_Delete(summary);
        AIRY_FREE(body);
        return;
    }
    snprintf(body, text_len, "%s%s", LG_PREFIX, context);
    cJSON_AddStringToObject(summary, "role", "user");
    cJSON_AddStringToObject(summary, "content", body);
    AIRY_FREE(body);

    int min_idx = -1;
    int msize = cJSON_GetArraySize(messages);
    for (int i = 0; i < msize; ++i) {
        if (lg_is_candidate(lg, protect, cJSON_GetArrayItem(messages, i))) {
            min_idx = i;
            break;
        }
    }
    if (min_idx < 0) {
        cJSON_Delete(summary);
        return;
    }

    /* 先删后插；插入失败以 msg=NULL 占位映射兜底，防悬空指针。 */
    for (int i = msize - 1; i >= 0; --i) {
        if (lg_is_candidate(lg, protect, cJSON_GetArrayItem(messages, i)))
            cJSON_DeleteItemFromArray(messages, i);
    }
    cJSON *inserted =
        cJSON_InsertItemInArray(messages, min_idx, summary) ? summary : NULL;
    if (!inserted)
        cJSON_Delete(summary);

    size_t keep = 1 + (lg->count - protect);
    agent_ledger_item_t *ni = (agent_ledger_item_t *)AIRY_MALLOC((keep + 1) * sizeof(*ni));
    if (!ni)
        return; /* 分配失败：映射截断为空，后续按无映射降级 */
    ni[0] = lg->items[0];
    size_t w = 1;
    for (size_t j = protect; j < lg->count; ++j)
        ni[w++] = lg->items[j];
    AIRY_FREE(lg->items);
    lg->items = ni;
    lg->cap = keep + 1;
    lg->count = w;
    lg_item_push(lg, "", "user", inserted);
    SVC_LOG_INFO("agent.run: context compressed (session=%s, entries=%d, at=%d)", lg->sess,
                 n_entries, min_idx);
}

void agent_ledger_fit(agent_ledger_t *lg, cJSON *messages)
{
    if (!lg->enabled || !messages)
        return;
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return;
    cJSON_AddStringToObject(params, "session_id", lg->sess);
    char *resp = lg_call(lg, "ledger_window", params);
    cJSON_Delete(params);
    if (!resp) {
        SVC_LOG_DEBUG("agent.run: ledger_window failed (session=%s)", lg->sess);
        return;
    }
    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    cJSON *warn = root ? cJSON_GetObjectItem(root, "warn") : NULL;
    if (!root || !cJSON_IsTrue(warn)) {
        cJSON_Delete(root);
        return;
    }
    size_t protect = lg_protect_start(lg);
    cJSON *entries = protect > 1 ? lg_cand_entries(lg, protect) : NULL;
    if (!entries) {
        cJSON_Delete(root);
        return;
    }
    cJSON *cparams = cJSON_CreateObject();
    if (!cparams) {
        cJSON_Delete(entries);
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(cparams, "session_id", lg->sess);
    cJSON_AddItemToObject(cparams, "entries", entries);
    int n_entries = cJSON_GetArraySize(entries);
    char *comp_resp = lg_call(lg, "compress", cparams);
    cJSON_Delete(cparams); /* entries 所有权已随 params 递归释放 */
    if (!comp_resp) {
        SVC_LOG_DEBUG("agent.run: mem.compress failed (session=%s)", lg->sess);
        cJSON_Delete(root);
        return;
    }
    cJSON *comp = cJSON_Parse(comp_resp);
    AIRY_FREE(comp_resp);
    cJSON *context = comp ? cJSON_GetObjectItem(comp, "context") : NULL;
    if (cJSON_IsString(context) && context->valuestring && context->valuestring[0])
        lg_apply(lg, messages, protect, context->valuestring, n_entries);
    cJSON_Delete(comp);
    cJSON_Delete(root);
}
