// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_task_orchestrate.c
 * @brief 统一任务编排入口的裁决与身份生成（0.1.21 C-15）。
 *
 * 服务端是 chat/task 的唯一裁决者：客户端只提交 goal 与可选的显式
 * 声明，规则命中结果（mode_reason）随事件与结果一并下发，客户端不再
 * 依据正文标记猜模式。本文件只做纯判定，不触达 LLM、工具或会话。
 */

#include "agent_task_contract.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "airy_verify.h"
#include "platform.h"
#include "safe_utf8.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* 资源操作动词：直接作用于某个既有资源（打开/关闭/播放……）。 */
static const char *const k_resource_verbs[] = {
    "打开", "启动", "运行", "关闭", "重启", "停止", "暂停", "播放", "截图", "拍照",
    "录屏", "下载", "上传", "挂载", "卸载", "连接", "断开", "切换", NULL,
};

/* 工程动词：会改动系统状态的构建类动作（安装/编译/部署……）。 */
static const char *const k_engineering_verbs[] = {
    "安装", "编译", "构建", "部署", "升级", "更新", "修复", "调试", "重构",
    "迁移", "配置", "创建", "生成", "删除", "打包", "发布", "实现", "开发", NULL,
};

/* 短对话标记：出现即判 chat，不受短文本长度启发式影响。 */
static const char *const k_chat_markers[] = {
    "你好", "您好", "在吗", "谢谢", "早上好", "晚上好", "再见", "你是谁",
    "hello", "hi", "thanks", "thank you", NULL,
};

static bool has_any_keyword(const char *goal, const char *const *keywords)
{
    for (size_t i = 0; keywords[i]; i++) {
        if (strstr(goal, keywords[i]))
            return true;
    }
    return false;
}

/* UTF-8 码点数（非法字节按单字符计，保证阈值判定不会无限循环）。 */
static size_t goal_char_count(const char *s)
{
    size_t len = strlen(s);
    size_t i = 0;
    size_t count = 0;
    while (i < len) {
        size_t width = 0;
        uint32_t cp = 0;
        if (utf8_decode((const unsigned char *)s + i, len - i, &width, &cp) && width > 0)
            i += width;
        else
            i += 1;
        (void)cp;
        count++;
    }
    return count;
}

int atk_decide_mode(const char *goal, const char *declared_mode, char *mode,
                           size_t mode_size, char *reason, size_t reason_size)
{
    if (!goal || !mode || !reason || mode_size == 0 || reason_size == 0)
        return -1;

    /* 1. 显式声明优先：客户端明确要求 chat/task 时不再跑规则。 */
    if (declared_mode && strcmp(declared_mode, AGENT_TASK_MODE_CHAT) == 0) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_CHAT, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_EXPLICIT_CHAT, reason_size);
        return 0;
    }
    if (declared_mode && strcmp(declared_mode, AGENT_TASK_MODE_TASK) == 0) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_TASK, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_EXPLICIT_TASK, reason_size);
        return 0;
    }

    /* 2. 长目标：有足够上下文即视为任务（先于动词规则，避免长文本误判 chat）。 */
    if (goal_char_count(goal) >= AGENT_TASK_LONG_GOAL_CHARS) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_TASK, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_LONG_GOAL, reason_size);
        return 0;
    }

    /* 3. 资源操作 / 工程动词：命中即任务。 */
    if (has_any_keyword(goal, k_resource_verbs)) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_TASK, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_RESOURCE_VERB, reason_size);
        return 0;
    }
    if (has_any_keyword(goal, k_engineering_verbs)) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_TASK, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_ENGINEERING_VERB, reason_size);
        return 0;
    }

    /* 4. 短对话：问候/致谢或极短且无动作词的输入。 */
    if (has_any_keyword(goal, k_chat_markers) ||
        goal_char_count(goal) <= AGENT_TASK_SHORT_CHAT_CHARS) {
        AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_CHAT, mode_size);
        AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_SHORT_CHAT, reason_size);
        return 0;
    }

    /* 5. 缺省 chat：没有任务证据时保持对话，避免误触发工具。 */
    AIRY_STRNCPY_TERM(mode, AGENT_TASK_MODE_CHAT, mode_size);
    AIRY_STRNCPY_TERM(reason, AGENT_TASK_REASON_DEFAULT_CHAT, reason_size);
    return 0;
}

void agent_task_gen_id(const char *prefix, char *out, size_t size)
{
    static atomic_uint seq;
    if (!out || size == 0)
        return;
    unsigned int n = atomic_fetch_add_explicit(&seq, 1u, memory_order_relaxed) + 1u;
    unsigned long long ts = (unsigned long long)(airy_time_ms() / 1000ull);
    snprintf(out, size, "%s-%llu-%u", prefix ? prefix : "task", ts, n);
}

/* GCCP 四槽：目标、上下文、约束、计划。答案以对象形态提交。 */
static const char *const k_gccp_slots[] = {"goal", "context", "constraints", "plan", NULL};

int agent_task_gccp_check(const cJSON *gccp_answers, bool *confirmed,
                          agent_task_missing_slot_t *missing, size_t max_missing,
                          size_t *missing_count)
{
    if (!confirmed || !missing || !missing_count)
        return -1;
    *missing_count = 0;

    /* 未提交答案：保持调用方声明值（CLI/TUI 缺省 false），missing 为空。 */
    if (!gccp_answers)
        return 0;
    if (!cJSON_IsObject(gccp_answers)) {
        *confirmed = false;
        return -1;
    }

    for (size_t i = 0; k_gccp_slots[i] && *missing_count < max_missing; i++) {
        cJSON *slot = cJSON_GetObjectItemCaseSensitive(gccp_answers, k_gccp_slots[i]);
        bool filled = cJSON_IsString(slot) ? (slot->valuestring && slot->valuestring[0] != '\0')
                                           : (slot && !cJSON_IsNull(slot));
        if (filled)
            continue;
        AIRY_STRNCPY_TERM(missing[*missing_count], k_gccp_slots[i], AGENT_TASK_MISSING_KEY_LEN);
        (*missing_count)++;
    }
    /* 答案齐备即视为已确认；任一缺失一律回到 needs_intent。 */
    *confirmed = (*missing_count == 0);
    return 0;
}

cJSON *agent_task_build_needs_intent(const agent_run_task_meta_t *meta,
                                     agent_task_missing_slot_t *missing,
                                     size_t missing_count)
{
    cJSON *out = cJSON_CreateObject();
    if (!out)
        return NULL;
    cJSON_AddStringToObject(out, AIRY_RS_K_STATUS, AGENT_TASK_STATUS_NEEDS_INTENT);
    cJSON_AddStringToObject(out, AIRY_RS_K_TASK_ID, meta ? meta->task_id : "");
    cJSON_AddStringToObject(out, AIRY_RS_K_MODE,
                            meta && meta->mode[0] ? meta->mode : AGENT_TASK_MODE_TASK);
    cJSON_AddStringToObject(out, AIRY_RS_K_MODE_REASON, meta ? meta->mode_reason : "");
    cJSON_AddBoolToObject(out, AIRY_RS_K_GCCP_CONFIRMED, false);
    cJSON_AddStringToObject(out, "message",
                            "goal not confirmed, provide gccp_answers before execution");
    if (meta && meta->success_criteria && meta->success_criteria[0])
        cJSON_AddStringToObject(out, AIRY_RS_K_SUCCESS_CRITERIA, meta->success_criteria);

    /* gccp_missing 为权威键；missing 为 285H 参考输出的兼容别名。 */
    cJSON *a = cJSON_AddArrayToObject(out, AIRY_RS_K_GCCP_MISSING);
    cJSON *b = cJSON_AddArrayToObject(out, "missing");
    for (size_t i = 0; missing && i < missing_count; i++) {
        if (a)
            cJSON_AddItemToArray(a, cJSON_CreateString(missing[i]));
        if (b)
            cJSON_AddItemToArray(b, cJSON_CreateString(missing[i]));
    }
    return out;
}

bool agent_task_gate_allows(const char *mode, bool gccp_confirmed)
{
    /* 唯一门禁条件：未确认的 task 不放行；chat 与已确认任务一律放行。
     * 关闭时调用方必须在引擎之前返回 needs_intent（gate:lab）。 */
    if (!mode || strcmp(mode, AGENT_TASK_MODE_TASK) != 0)
        return true;
    return gccp_confirmed;
}

/* ---- 完成判据（C-17） ---- */

/* 单条判据的结构合法性：type 决定唯一必需字段，缺一即不可复核。 */
static bool criteria_item_ok(const cJSON *it)
{
    if (!cJSON_IsObject(it))
        return false;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_TYPE);
    if (!cJSON_IsString(t) || !t->valuestring)
        return false;

    const cJSON *need = NULL;
    if (strcmp(t->valuestring, AIRY_VC_T_CMD) == 0) {
        need = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_CMD);
    } else if (strcmp(t->valuestring, AIRY_VC_T_FILE) == 0) {
        need = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_PATH);
    } else if (strcmp(t->valuestring, AIRY_VC_T_SERVICE) == 0) {
        need = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_PORT);
        if (need && cJSON_IsNumber(need) && need->valueint > 0 && need->valueint <= 65535)
            return true;
        return false;
    } else if (strcmp(t->valuestring, AIRY_VC_T_RPC) == 0) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_METHOD);
        need = cJSON_GetObjectItemCaseSensitive(it, AIRY_VC_DAEMON);
        return cJSON_IsString(need) && need->valuestring && need->valuestring[0] &&
               cJSON_IsString(m) && m->valuestring && m->valuestring[0];
    } else {
        return false;
    }
    return cJSON_IsString(need) && need->valuestring && need->valuestring[0];
}

/* 取出判据数组（兼容裸数组与 {"criteria":[...]} 两种线形态）。 */
static const cJSON *criteria_array(const cJSON *root)
{
    if (!root)
        return NULL;
    return cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, AIRY_VK_CRITERIA);
}

bool agent_task_criteria_ok(const char *criteria)
{
    cJSON *root = (criteria && criteria[0]) ? cJSON_Parse(criteria) : NULL;
    if (!root)
        return false;

    const cJSON *arr = criteria_array(root);
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    bool ok = n >= 1 && n <= AGENT_TASK_CRITERIA_MAX;
    for (int i = 0; ok && i < n; i++)
        ok = criteria_item_ok(cJSON_GetArrayItem(arr, i));
    cJSON_Delete(root);
    return ok;
}

/* 把一条 postcondition 收进判据数组：显式对象逐项校验，数组逐项展开，
 * 其余形态（含自然语言字符串）一律跳过。 */
static void plan_post_append(cJSON *dst, const cJSON *post)
{
    if (cJSON_IsObject(post)) {
        if (criteria_item_ok(post) && cJSON_GetArraySize(dst) < AGENT_TASK_CRITERIA_MAX)
            cJSON_AddItemToArray(dst, cJSON_Duplicate(post, 1));
        return;
    }
    if (!cJSON_IsArray(post))
        return;
    int n = cJSON_GetArraySize(post);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(post, i);
        if (criteria_item_ok(it) && cJSON_GetArraySize(dst) < AGENT_TASK_CRITERIA_MAX)
            cJSON_AddItemToArray(dst, cJSON_Duplicate(it, 1));
    }
}

char *criteria_from_plan(const cJSON *plan)
{
    const cJSON *steps =
        cJSON_IsObject(plan) ? cJSON_GetObjectItemCaseSensitive(plan, "steps") : plan;
    if (!cJSON_IsArray(steps))
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    int n = cJSON_GetArraySize(steps);
    for (int i = 0; i < n && cJSON_GetArraySize(arr) < AGENT_TASK_CRITERIA_MAX; i++) {
        const cJSON *step = cJSON_GetArrayItem(steps, i);
        plan_post_append(arr, cJSON_GetObjectItemCaseSensitive(step, "postcondition"));
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

cJSON *need_criteria_of(const agent_run_task_meta_t *meta, const char *why)
{
    cJSON *out = cJSON_CreateObject();
    if (!out)
        return NULL;
    cJSON_AddStringToObject(out, AIRY_RS_K_STATUS, AGENT_TASK_STATUS_NEEDS_CRITERIA);
    if (meta) {
        cJSON_AddStringToObject(out, AIRY_RS_K_TASK_ID, meta->task_id);
        cJSON_AddStringToObject(out, AIRY_RS_K_MODE, meta->mode);
        cJSON_AddStringToObject(out, AIRY_RS_K_MODE_REASON, meta->mode_reason);
        cJSON_AddBoolToObject(out, AIRY_RS_K_GCCP_CONFIRMED, meta->gccp_confirmed);
    }
    cJSON_AddStringToObject(out, "message",
                            why && why[0] ? why
                                          : "task mode requires at least one success criterion");
    /* 补判据示例：一条文件判据 + 一条命令判据，均为只读取证，客户端可
     * 直接照抄成 success_criteria 后重提。 */
    cJSON *ex = cJSON_AddObjectToObject(out, "example");
    cJSON *arr = ex ? cJSON_AddArrayToObject(ex, AIRY_VK_CRITERIA) : NULL;
    if (arr) {
        cJSON *f = cJSON_CreateObject();
        if (f) {
            cJSON_AddStringToObject(f, AIRY_VC_TYPE, AIRY_VC_T_FILE);
            cJSON_AddStringToObject(f, AIRY_VC_PATH, "out/result.txt");
            cJSON_AddStringToObject(f, AIRY_VC_CONTAINS, "done");
            cJSON_AddItemToArray(arr, f);
        }
        cJSON *s = cJSON_CreateObject();
        if (s) {
            cJSON_AddStringToObject(s, AIRY_VC_TYPE, AIRY_VC_T_CMD);
            cJSON_AddStringToObject(s, AIRY_VC_CMD, "test -f out/result.txt");
            cJSON_AddItemToArray(arr, s);
        }
    }
    return out;
}
