// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file llm_daemon_request.c
 * @brief llm_d daemon request-parsing domain (split from main.c,
 *        2026-08-27): host-time context injection, request-context
 *        lifecycle and JSON-RPC params parsing.
 *
 * 2026-08-27 域拆分（main.c 1033 行 → 4 文件）：
 *   - main.c                入口引导：daemon 宏实例化、信号接线、方法注册
 *   - llm_daemon_request.c  请求解析域（本文件）
 *   - llm_daemon_methods.c  RPC 方法域（complete/embeddings/流式等）
 *   - llm_daemon_config.c   配置装配域（daemon 配置加载与服务销毁）
 *
 * 跨文件共享符号经 llm_service_internal.h 声明；daemon_main.h 生成的
 * static 样板（g_running_llm_d 等）仍留在 main.c 内。
 */

#include "airy_memory.h"
#include "error.h"
#include "llm_service_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* P0.18.1: transitively provides SVC_LOG_*, CJSON_PARSE_GUARD and the sock/
 * logger/event-driver declarations used below. */
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

    ctx->response_capacity = MAX_BUFFER;
    ctx->response_buffer = (char *)AIRY_MALLOC(ctx->response_capacity);
    if (!ctx->response_buffer) {
        AIRY_FREE(ctx);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    ctx->response_buffer[0] = '\0';
    ctx->response_size = 0;

    return ctx;
}

/**
 * @brief Destroy the request context
 */
void request_context_destroy(request_context_t *ctx)
{
    if (!ctx)
        return;

    for (size_t i = 0; i < ctx->message_count; i++) {
        AIRY_FREE((void *)ctx->messages[i].role);
        AIRY_FREE((void *)ctx->messages[i].content);
        AIRY_FREE((void *)ctx->messages[i].reasoning_content);
        AIRY_FREE((void *)ctx->messages[i].tool_call_id);
        AIRY_FREE((void *)ctx->messages[i].tool_calls_json);
    }

    AIRY_FREE(ctx->tools_json);
    AIRY_FREE(ctx->response_buffer);
    AIRY_FREE(ctx);
}

/**
 * @brief Parse request params into llm_request_config_t
 * @param params JSON params object
 * @param ctx    Request context (used to store messages)
 * @param cfg    Output config
 * @return 0 on success, non-zero on failure
 */
static void parse_params_cleanup(request_context_t *ctx, llm_request_config_t *cfg)
{
    if (cfg->model) {
        AIRY_FREE((void *)cfg->model);
        cfg->model = NULL;
    }
    for (size_t i = 0; i < ctx->message_count; i++) {
        AIRY_FREE((void *)ctx->messages[i].role);
        AIRY_FREE((void *)ctx->messages[i].content);
        AIRY_FREE((void *)ctx->messages[i].reasoning_content);
        AIRY_FREE((void *)ctx->messages[i].tool_call_id);
        AIRY_FREE((void *)ctx->messages[i].tool_calls_json);
        ctx->messages[i].role = NULL;
        ctx->messages[i].content = NULL;
        ctx->messages[i].reasoning_content = NULL;
        ctx->messages[i].tool_call_id = NULL;
        ctx->messages[i].tool_calls_json = NULL;
    }
    ctx->message_count = 0;
    AIRY_FREE(ctx->tools_json);
    ctx->tools_json = NULL;
}

int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg)
{
    __builtin_memset(cfg, 0, sizeof(llm_request_config_t));

    cJSON *model = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(model)) {
        cfg->model = AIRY_STRDUP(model->valuestring);
    } else {
        /* A2-2: when model is omitted, fall back to global.default_model
         * (previously llm_d had no default concept; an empty model always
         * returned INVALID_PARAM, forcing clients to specify a model) */
        const char *def = g_service ? llm_service_default_model(g_service) : NULL;
        if (def) {
            cfg->model = AIRY_STRDUP(def);
        } else {
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM,
                       "model parameter is not a string and no default model configured");
        }
    }
    if (!cfg->model) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate model string");
    }

    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) == 0) {
        parse_params_cleanup(ctx, cfg);
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "messages must be a non-empty array");
    }
    if (cJSON_IsArray(messages)) {
        size_t count = cJSON_GetArraySize(messages);
        if (count > MAX_MESSAGES_PER_REQUEST) {
            parse_params_cleanup(ctx, cfg);
            AIRY_ERROR(AIRY_ERR_OVERFLOW, "too many messages");
        }

        /* 时间感知注入（2026-08-17）：把宿主机当前时间作为 system 消息
         * 注入到消息数组头部。模型据此感知"今天/现在/星期几"，多轮上下文
         * 拼接不再时间漂移。仅当首条 system 消息已包含**真实日期时间戳**
         * （YYYY-MM-DD 形式，客户端主动注入）时跳过；提示词中仅提及
         * "当前时间"字样（如 CLI 系统提示词描述"系统上下文已注入当前时间"）
         * 不代表真实时间已注入，仍须补注，避免模型幻觉。 */
        int time_injected = 0;
        if (count > 0) {
            cJSON *first = cJSON_GetArrayItem(messages, 0);
            cJSON *frole = cJSON_GetObjectItem(first, "role");
            cJSON *fcontent = cJSON_GetObjectItem(first, "content");
            if (cJSON_IsString(frole) && strcmp(frole->valuestring, "system") == 0 &&
                cJSON_IsString(fcontent) && fcontent->valuestring &&
                fcontent->valuestring[0]) {
                /* 检测真实日期时间戳：YYYY-MM-DD（客户端注入的时间上下文
                 * 形如 "当前时间：2026-08-17 星期一 23:18 ..."）。 */
                const char *p = fcontent->valuestring;
                for (; *p; ++p) {
                    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                        p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' &&
                        p[4] == '-' && p[5] >= '0' && p[5] <= '9' && p[6] >= '0' &&
                        p[6] <= '9' && p[7] == '-' && p[8] >= '0' && p[8] <= '9' &&
                        p[9] >= '0' && p[9] <= '9') {
                        time_injected = 1;
                        break;
                    }
                }
            }
        }
        size_t base = 0;
        if (!time_injected) {
            if (count + 1 > MAX_MESSAGES_PER_REQUEST) {
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_OVERFLOW, "too many messages");
            }
            char tbuf[LLM_TIME_CTX_CAP];
            llm_build_time_context(tbuf, sizeof(tbuf));
            ctx->messages[0].role = AIRY_STRDUP("system");
            ctx->messages[0].content = AIRY_STRDUP(tbuf);
            if (!ctx->messages[0].role || !ctx->messages[0].content) {
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to inject time context");
            }
            base = 1;
            SVC_LOG_DEBUG("C-L02: SVC: injected host time context: %s", tbuf);
        }

        ctx->message_count = count + base;
        cfg->message_count = count + base;
        cfg->messages = ctx->messages;

        for (size_t i = 0; i < count; ++i) {
            size_t slot = i + base;
            cJSON *item = cJSON_GetArrayItem(messages, i);
            cJSON *role = cJSON_GetObjectItem(item, "role");
            cJSON *content = cJSON_GetObjectItem(item, "content");

            if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "message role or content is not a string");
            }

            ctx->messages[slot].role = AIRY_STRDUP(role->valuestring);
            ctx->messages[slot].content = AIRY_STRDUP(content->valuestring);

            if (!ctx->messages[slot].role || !ctx->messages[slot].content) {
                ctx->message_count = slot;
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate message role or content");
            }

            cJSON *reasoning = cJSON_GetObjectItem(item, "reasoning_content");
            if (cJSON_IsString(reasoning) && reasoning->valuestring && reasoning->valuestring[0]) {
                ctx->messages[slot].reasoning_content = AIRY_STRDUP(reasoning->valuestring);
                if (!ctx->messages[slot].reasoning_content) {
                    ctx->message_count = slot + 1;
                    parse_params_cleanup(ctx, cfg);
                    AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate reasoning_content");
                }
            }

            cJSON *tcid = cJSON_GetObjectItem(item, "tool_call_id");
            if (cJSON_IsString(tcid) && tcid->valuestring && tcid->valuestring[0]) {
                ctx->messages[slot].tool_call_id = AIRY_STRDUP(tcid->valuestring);
                if (!ctx->messages[slot].tool_call_id) {
                    ctx->message_count = slot + 1;
                    parse_params_cleanup(ctx, cfg);
                    AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate tool_call_id");
                }
            }

            cJSON *tcalls = cJSON_GetObjectItem(item, "tool_calls");
            if (cJSON_IsArray(tcalls) && cJSON_GetArraySize(tcalls) > 0) {
                ctx->messages[slot].tool_calls_json = cJSON_PrintUnformatted(tcalls);
                SVC_LOG_INFO("C-L02: SVC: msg[%zu] role=%s tool_calls=%.400s", slot,
                             ctx->messages[slot].role ? ctx->messages[slot].role : "?",
                             ctx->messages[slot].tool_calls_json ? ctx->messages[slot].tool_calls_json
                                                                 : "(null)");
                if (!ctx->messages[slot].tool_calls_json) {
                    ctx->message_count = slot + 1;
                    parse_params_cleanup(ctx, cfg);
                    AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to serialize tool_calls");
                }
            }
        }
    }

    cJSON *temp = cJSON_GetObjectItem(params, "temperature");
    if (cJSON_IsNumber(temp)) {
        cfg->temperature = (float)temp->valuedouble;
    }

    cJSON *top_p = cJSON_GetObjectItem(params, "top_p");
    if (cJSON_IsNumber(top_p)) {
        cfg->top_p = (float)top_p->valuedouble;
    }

    cJSON *max_tokens = cJSON_GetObjectItem(params, "max_tokens");
    if (cJSON_IsNumber(max_tokens)) {
        cfg->max_tokens = max_tokens->valueint;
    }

    cJSON *stream = cJSON_GetObjectItem(params, "stream");
    if (cJSON_IsBool(stream)) {
        cfg->stream = cJSON_IsTrue(stream) ? 1 : 0;
    }

    cJSON *presence_penalty = cJSON_GetObjectItem(params, "presence_penalty");
    if (cJSON_IsNumber(presence_penalty)) {
        cfg->presence_penalty = presence_penalty->valuedouble;
    }

    cJSON *frequency_penalty = cJSON_GetObjectItem(params, "frequency_penalty");
    if (cJSON_IsNumber(frequency_penalty)) {
        cfg->frequency_penalty = frequency_penalty->valuedouble;
    }

    cJSON *tools = cJSON_GetObjectItem(params, "tools");
    if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0) {
        ctx->tools_json = cJSON_PrintUnformatted(tools);
        if (!ctx->tools_json) {
            parse_params_cleanup(ctx, cfg);
            AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to serialize tools");
        }
        cfg->tools_json = ctx->tools_json;
    }

    return 0;
}
