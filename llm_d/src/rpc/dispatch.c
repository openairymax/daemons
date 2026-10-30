// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file dispatch.c
 * @brief llm_d 请求解析域：宿主机时间上下文注入、请求上下文生命周期与
 *        JSON-RPC params 解析。
 *
 * 0.1.19 gen5 装配：本文件与 src/rpc/methods.c（方法域）、src/svc.c（生命周期
 * 与端点）同属 llm_d 装配域，共享符号经 llm_d_internal.h 声明；daemon_main.h
 * 生成的 static 样板（g_running_llm_d 等）留在生成态 src/main.c 内。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_llm_d.h"
#include "llm_d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 传递提供 SVC_LOG_*、CJSON_PARSE_GUARD 及下方使用的 sock/logger/
 * event-driver 声明。 */
#include "daemon_main.h"

#define LLM_TIME_CTX_CAP 256
/**
 * @brief Build a "current host time" system context string
 *
 * 时间感知（2026-08-17）：agentrt 必须准确识别宿主机系统时间——模型对
 * "今天/现在/几点了/星期几/时效性" 等问题的回答依赖此上下文，且多轮对话
 * 拼接时时间漂移会让模型产生幻觉。注入格式（含时区与星期，便于模型理解）：
 *   当前时间：2026-08-17 星期一 22:59（Asia/Shanghai）
 * @param out 输出缓冲（>= LLM_TIME_CTX_CAP）
 * @param cap 缓冲容量
 */
static void llm_build_time_context(char *out, size_t cap)
{
    out[0] = '\0';
    if (!out || cap < 8)
        return;
    /* 宿主机本地时间（TZ 环境变量决定时区，默认 Asia/Shanghai） */
    time_t now = time(NULL);
    struct tm tmv;
    AIRY_MEMSET(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char when[48];
    /* %z 时区偏移；无偏移时回退为空 */
    size_t used = strftime(when, sizeof(when), "%Y-%m-%d %A %H:%M (%z)", &tmv);
    if (used == 0) {
        AIRY_STRNCPY_TERM(when, "unknown", sizeof(when));
    }
    /* 时区偏移（%z 输出 +0800）转成可读 IANA 风格：+0800 → UTC+8 */
    char tz_hint[24] = "";
    if (used > 0 && strlen(when) >= 5) {
        char *paren = strrchr(when, '(');
        if (paren && strlen(paren) >= 7) {
            char off[8];
            AIRY_STRNCPY_TERM(off, paren + 1, sizeof(off));
            /* 移除右括号 */
            size_t ol = strlen(off);
            if (ol > 0 && off[ol - 1] == ')')
                off[ol - 1] = '\0';
            if (strlen(off) == 5 && (off[0] == '+' || off[0] == '-')) {
                char sign = off[0];
                char hh[3] = {off[1], off[2], '\0'};
                snprintf(tz_hint, sizeof(tz_hint), "UTC%c%d", sign, atoi(hh));
            }
        }
    }
    /* 注入文案要足够强势：模型有 web_search 等实时工具时，若只说"以此为准"
     * 仍可能为查时间去联网（CLI 实测绕 web 查时间且网络时间缓存冲突易错）。
     * 明确指令：时间类问题直接用本时间作答，禁止为查时间调用工具。 */
    snprintf(out, cap,
             "当前时间：%s%s%s（宿主机系统时间；回答时间相关问题时以此为准，"
             "直接使用该时间作答，禁止为查询时间调用任何工具）",
             when, tz_hint[0] ? " " : "", tz_hint);
}

/**
 * @brief Create the request context
 */
request_context_t *request_context_create(void)
{
    request_context_t *ctx = (request_context_t *)AIRY_CALLOC(1, sizeof(request_context_t));
    if (!ctx) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    return ctx;
}

/* 释放已装入的 messages 并将计数归零：计数归零即关闭重复释放窗口，
 * 后续 destroy 不会再次遍历同一批指针。 */
static void messages_release(request_context_t *ctx)
{
    for (size_t i = 0; i < ctx->message_count; i++) {
        AIRY_FREE((void *)ctx->messages[i].role);
        AIRY_FREE((void *)ctx->messages[i].content);
        AIRY_FREE((void *)ctx->messages[i].reasoning_content);
        AIRY_FREE((void *)ctx->messages[i].tool_call_id);
        AIRY_FREE((void *)ctx->messages[i].tool_calls_json);
    }
    ctx->message_count = 0;
}

/**
 * @brief Destroy the request context
 */
void request_context_destroy(request_context_t *ctx)
{
    if (!ctx)
        return;

    messages_release(ctx);
    AIRY_FREE(ctx->tools_json);
    AIRY_FREE(ctx->grammar);
    AIRY_FREE(ctx);
}
/**
 * @brief Parse request params into llm_request_config_t
 *
 * 按字段域拆成独立阶段，任一阶段失败都经 msg_fail 统一回收，半装入状态
 * 不外泄。
 */

/* 解析域的栈上上下文：ctx/cfg 双指针收拢为单参数，使各阶段签名与失败
 * 出口保持单行。 */
typedef struct {
    request_context_t *ctx;
    llm_request_config_t *cfg;
} req_env_t;

/* 释放本轮已装入的 model / messages / tools；消息计数一并归零，关闭
 * 重复释放窗口。 */
static void req_cleanup(req_env_t *r)
{
    AIRY_FREE((void *)r->cfg->model);
    r->cfg->model = NULL;
    messages_release(r->ctx);
    AIRY_FREE(r->ctx->tools_json);
    r->ctx->tools_json = NULL;
    AIRY_FREE(r->ctx->grammar);
    r->ctx->grammar = NULL;
    r->cfg->grammar = NULL;
}

/* 解析失败的统一出口：先把消息计数收敛到 keep（半填槽位纳入回收上界），
 * 再释放已分配资源，并把具体环节写入 fail_reason。 */
static int msg_fail(req_env_t *r, size_t keep, int code, const char *msg)
{
    r->ctx->message_count = keep;
    req_cleanup(r);
    snprintf(r->ctx->fail_reason, sizeof(r->ctx->fail_reason), "%s", msg);
    AIRY_ERROR(code, msg);
}

static int is_dg(char c)
{
    return c >= '0' && c <= '9';
}

/* 首条内容出现 YYYY-MM-DD 即视为真实日期戳，命中即返回（合法性由注入方
 * 负责）。按长度界定扫描窗口，避免越过字符串尾部读取。 */
static int has_date_ts(const char *p)
{
    size_t n = strlen(p);
    for (size_t i = 0; i + 10 <= n; ++i)
        if (is_dg(p[i]) && is_dg(p[i + 1]) && is_dg(p[i + 2]) && is_dg(p[i + 3]) &&
            p[i + 4] == '-' && is_dg(p[i + 5]) && is_dg(p[i + 6]) && p[i + 7] == '-' &&
            is_dg(p[i + 8]) && is_dg(p[i + 9]))
            return 1;
    return 0;
}

/* 首条 system 消息是否已带真实日期时间戳。提示词里仅提及"当前时间"字样
 * 不算注入，仍须补注，否则模型会按训练期时间作答。 */
static int msg_has_time(cJSON *messages)
{
    cJSON *first = cJSON_GetArrayItem(messages, 0);
    cJSON *role = cJSON_GetObjectItem(first, "role");
    cJSON *text = cJSON_GetObjectItem(first, "content");
    if (!cJSON_IsString(role) || strcmp(role->valuestring, "system") != 0)
        return 0;
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0])
        return 0;
    return has_date_ts(text->valuestring);
}

/* 时间感知注入：宿主机当前时间作为 system 消息占据 slot 0，其后消息整体
 * 后移一位（*base = 1）。 */
static int inject_time(req_env_t *r, size_t count, size_t *base)
{
    if (count + 1 > MAX_MESSAGES_PER_REQUEST)
        return msg_fail(r, 0, AIRY_ERR_OVERFLOW, "too many messages");
    char tbuf[LLM_TIME_CTX_CAP];
    llm_build_time_context(tbuf, sizeof(tbuf));
    r->ctx->messages[0].role = AIRY_STRDUP("system");
    r->ctx->messages[0].content = AIRY_STRDUP(tbuf);
    if (!r->ctx->messages[0].role || !r->ctx->messages[0].content)
        return msg_fail(r, 1, AIRY_ERR_OUT_OF_MEMORY, "failed to inject time context");
    *base = 1;
    SVC_LOG_DEBUG("C-L02: SVC: injected host time context: %s", tbuf);
    return 0;
}

/* T1 前缀稳定化（方案 §5.9.3）：时间上下文追加为末尾独立 system 消息。
 * 服务端 KV 缓存按 token 前缀匹配，slot 0 注入使每轮首条随时间漂移、
 * 全量重算 prefill；尾部注入保住 system+历史的逐字节共同前缀，新增
 * prefill 只有时间消息本身。上限不足即拒绝，不静默丢弃时间感知。 */
static int append_time_tail(req_env_t *r, size_t slot)
{
    if (slot + 1 > MAX_MESSAGES_PER_REQUEST)
        return msg_fail(r, slot, AIRY_ERR_OVERFLOW, "too many messages");
    char tbuf[LLM_TIME_CTX_CAP];
    llm_build_time_context(tbuf, sizeof(tbuf));
    r->ctx->messages[slot].role = AIRY_STRDUP("system");
    r->ctx->messages[slot].content = AIRY_STRDUP(tbuf);
    if (!r->ctx->messages[slot].role || !r->ctx->messages[slot].content)
        return msg_fail(r, slot + 1, AIRY_ERR_OUT_OF_MEMORY,
                        "failed to inject time context");
    r->ctx->message_count = slot + 1;
    r->cfg->message_count = slot + 1;
    SVC_LOG_DEBUG("C-L02: SVC: appended host time context as tail: %s", tbuf);
    return 0;
}

/* T1 回退开关（§7.6 环境变量通道）：AIRY_LLM_PREFIX_STABLE=off|0 回退
 * slot 0 注入（0.1.19 行为）；缺省开启尾部注入。 */
static int llm_prefix_stable(void)
{
    const char *v = getenv("AIRY_LLM_PREFIX_STABLE");
    return !(v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0));
}

/* 可选字符串字段：仅非空字符串才复制；返回 -1 表示分配失败。 */
static int dup_opt(cJSON *item, const char *key, const char **dst)
{
    cJSON *v = cJSON_GetObjectItem(item, key);
    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0])
        return 0;
    *dst = AIRY_STRDUP(v->valuestring);
    return *dst ? 0 : -1;
}

/* 单个消息槽：role/content 必填，reasoning_content / tool_call_id /
 * tool_calls 可选。分配失败以 slot + 1 为回收上界，半填槽位一并释放。 */
static int fill_msg(req_env_t *r, cJSON *item, size_t slot)
{
    llm_message_t *m = &r->ctx->messages[slot];
    cJSON *role = cJSON_GetObjectItem(item, "role");
    cJSON *content = cJSON_GetObjectItem(item, "content");
    if (!cJSON_IsString(role) || !cJSON_IsString(content))
        return msg_fail(r, slot + 1, AIRY_ERR_INVALID_PARAM, "invalid message role/content");
    m->role = AIRY_STRDUP(role->valuestring);
    m->content = AIRY_STRDUP(content->valuestring);
    if (!m->role || !m->content)
        return msg_fail(r, slot + 1, AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate role/content");
    if (dup_opt(item, "reasoning_content", &m->reasoning_content) != 0)
        return msg_fail(r, slot + 1, AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate reasoning");
    if (dup_opt(item, "tool_call_id", &m->tool_call_id) != 0)
        return msg_fail(r, slot + 1, AIRY_ERR_OUT_OF_MEMORY, "failed to dup tool_call_id");
    cJSON *tcalls = cJSON_GetObjectItem(item, "tool_calls");
    if (!cJSON_IsArray(tcalls) || cJSON_GetArraySize(tcalls) == 0)
        return 0;
    m->tool_calls_json = cJSON_PrintUnformatted(tcalls);
    if (!m->tool_calls_json)
        return msg_fail(r, slot + 1, AIRY_ERR_OUT_OF_MEMORY, "failed to serialize tool_calls");
    SVC_LOG_INFO("C-L02: SVC: msg[%zu] role=%s tool_calls=%.400s", slot,
                 m->role ? m->role : "?", m->tool_calls_json);
    return 0;
}

/* 消息数组装配：判定一次是否补注时间上下文，stable 走尾部追加
 * （前缀稳定），回退走 slot 0 注入。计数在填充前一次性置位，使
 * fill_msg 的失败上界不大于该值。 */
static int fill_msgs(req_env_t *r, cJSON *messages, size_t count)
{
    size_t base = 0;
    int stable = llm_prefix_stable();
    int has_time = msg_has_time(messages);
    if (!stable && !has_time) {
        int rc = inject_time(r, count, &base);
        if (rc != 0)
            return rc;
    }
    r->ctx->message_count = count + base;
    r->cfg->message_count = count + base;
    r->cfg->messages = r->ctx->messages;
    for (size_t i = 0; i < count; ++i) {
        int rc = fill_msg(r, cJSON_GetArrayItem(messages, i), i + base);
        if (rc != 0)
            return rc;
    }
    if (stable && !has_time)
        return append_time_tail(r, count + base);
    return 0;
}

/* messages 参数：必须是非空数组且不超过每请求上限。 */
static int parse_messages(req_env_t *r, cJSON *params)
{
    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    size_t count = cJSON_IsArray(messages) ? (size_t)cJSON_GetArraySize(messages) : 0;
    if (count == 0)
        return msg_fail(r, 0, AIRY_ERR_INVALID_PARAM, "messages must be a non-empty array");
    if (count > MAX_MESSAGES_PER_REQUEST)
        return msg_fail(r, 0, AIRY_ERR_OVERFLOW, "too many messages");
    return fill_msgs(r, messages, count);
}

/* model 参数：显式给出则复制，缺省回落到 global.default_model。 */
static int parse_model(req_env_t *r, cJSON *params)
{
    cJSON *m = cJSON_GetObjectItem(params, "model");
    const char *src = cJSON_IsString(m) ? m->valuestring
                      : (g_service ? llm_service_default_model(g_service) : NULL);
    if (!src)
        return msg_fail(r, 0, AIRY_ERR_INVALID_PARAM, "model missing and no default");
    r->cfg->model = AIRY_STRDUP(src);
    if (!r->cfg->model)
        return msg_fail(r, 0, AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate model string");
    return 0;
}

/* 数值参数：类型相符才覆盖，缺省值由 parse_params 起手的 memset 给出。 */
static double num_or(cJSON *params, const char *key, double dflt)
{
    cJSON *v = cJSON_GetObjectItem(params, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

/* 纯标量组：num_or 只覆盖显式且类型相符的项，其余保持入参原值。 */
static void samp_scalar(llm_request_config_t *cfg, cJSON *params)
{
    cfg->top_p = (float)num_or(params, "top_p", cfg->top_p);
    cfg->max_tokens = (int)num_or(params, "max_tokens", cfg->max_tokens);
    cfg->presence_penalty = num_or(params, "presence_penalty", cfg->presence_penalty);
    cfg->frequency_penalty = num_or(params, "frequency_penalty", cfg->frequency_penalty);
    cfg->seed = (uint32_t)num_or(params, "seed", cfg->seed);
    cfg->num_ctx = (uint32_t)num_or(params, "num_ctx", cfg->num_ctx);
}

/* 布尔组：只有明确给了 JSON bool 才改写，字符串 "true" 不算。 */
static void samp_flag(llm_request_config_t *cfg, cJSON *params)
{
    cJSON *v = cJSON_GetObjectItem(params, "stream");

    if (cJSON_IsBool(v))
        cfg->stream = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(params, "cacheable");
    if (cJSON_IsBool(v))
        cfg->cacheable = cJSON_IsTrue(v);
}

/* grammar 本体归 ctx 持有：请求结束后 cfg 失效但 ctx 仍要活到生成结束。 */
static int samp_grammar(req_env_t *r, cJSON *params)
{
    cJSON *v = cJSON_GetObjectItem(params, "grammar");

    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0])
        return 0;
    r->ctx->grammar = AIRY_STRDUP(v->valuestring);
    if (!r->ctx->grammar)
        return msg_fail(r, r->ctx->message_count, AIRY_ERR_OUT_OF_MEMORY,
                        "failed to duplicate grammar string");
    r->cfg->grammar = r->ctx->grammar;
    return 0;
}

/* 形状受控的两组：response_format 只认 json_object，槽位与 thinking 以
 * 哨兵记录"被设置过"，使 false 与 0 可区别于缺省。 */
static void samp_ext(llm_request_config_t *cfg, cJSON *params)
{
    cJSON *v = cJSON_GetObjectItem(params, "response_format");

    if (cJSON_IsObject(v)) {
        cJSON *t = cJSON_GetObjectItem(v, "type");
        if (cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, "json_object") == 0)
            cfg->response_format = LLM_RF_JSON_OBJECT;
    }

    /* T2 槽位亲和：显式传 slot_id 才置哨兵，槽位 0 合法可表达。 */
    v = cJSON_GetObjectItem(params, "slot_id");
    if (cJSON_IsNumber(v)) {
        cfg->slot_id = (int)v->valuedouble;
        cfg->slot_set = true;
    }

    /* C3 thinking 控制：显式传 chat_template_kwargs 才置哨兵，enable_thinking
     * 为意图载体（false 合法）；budget 可选，0 = 不发送。能力位门控与
     * chat_template_kwargs 线格式归 envelope 机制面，此处只解析调用方意图
     * （atoms 客户端 build_llm_request_json 同名同构透传）。 */
    v = cJSON_GetObjectItem(params, "chat_template_kwargs");
    if (!cJSON_IsObject(v))
        return;
    cJSON *et = cJSON_GetObjectItem(v, "enable_thinking");
    if (!cJSON_IsBool(et))
        return;

    cfg->enable_thinking = cJSON_IsTrue(et);
    cfg->thinking_set = true;
    cJSON *tb = cJSON_GetObjectItem(v, "thinking_budget");
    if (cJSON_IsNumber(tb) && tb->valuedouble >= 0.0)
        cfg->thinking_budget = (uint32_t)tb->valuedouble;
}

/* 采样参数：cacheable 缺省 0 即 B5-3 的 fail-closed，此处只覆盖显式且
 * 类型相符的项。C11 新参数遵零值=不发送约定；temperature 以显式哨兵
 * 记录"被设置过"，使 0.0 可区别于缺省。 */
static int parse_sampling(req_env_t *r, cJSON *params)
{
    llm_request_config_t *cfg = r->cfg;
    cJSON *v = cJSON_GetObjectItem(params, "temperature");

    if (cJSON_IsNumber(v)) {
        cfg->temperature = (float)v->valuedouble;
        cfg->temperature_set = true;
    }
    samp_scalar(cfg, params);
    samp_flag(cfg, params);
    samp_ext(cfg, params);
    return samp_grammar(r, params);
}

/* tools 参数：序列化后由 ctx 持有所有权，cfg 只借用指针。 */
static int parse_tools(req_env_t *r, cJSON *params)
{
    cJSON *tools = cJSON_GetObjectItem(params, "tools");
    if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) == 0)
        return 0;
    r->ctx->tools_json = cJSON_PrintUnformatted(tools);
    if (!r->ctx->tools_json)
        return msg_fail(r, r->ctx->message_count, AIRY_ERR_OUT_OF_MEMORY,
                        "failed to serialize tools");
    r->cfg->tools_json = r->ctx->tools_json;
    return 0;
}

int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg)
{
    __builtin_memset(cfg, 0, sizeof(llm_request_config_t));
    req_env_t r = {ctx, cfg};

    int rc = parse_model(&r, params);
    if (rc != 0)
        return rc;
    rc = parse_messages(&r, params);
    if (rc != 0)
        return rc;
    rc = parse_sampling(&r, params);
    if (rc != 0)
        return rc;
    return parse_tools(&r, params);
}
