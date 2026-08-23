// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief LLM service daemon main entry (daemon module conventions).
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 */

/* P0.18.1: daemon_main.h provides DAEMON_DECLARE_COMMON/DAEMON_SETUP_SIGNALS/
 * daemon_parse_args/daemon_create_server_socket/daemon_init_event_driver/
 * daemon_cleanup_standard boilerplate macros and inline helpers. It
 * transitively includes atomic_compat.h, daemon_bootstrap_*.h,
 * daemon_cupolas_bootstrap.h, daemon_event_driver.h, daemon_platform_ext.h,
 * jsonrpc_helpers.h, logging.h, method_dispatcher.h, svc_logger.h,
 * cjson/cJSON.h, cjson_helpers.h, so only business-logic headers are kept
 * here. */
#include "llm_service.h"
#include "response.h"
#include "token_counter.h"
#include "daemon_main.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* macOS/BSD 无 MSG_NOSIGNAL（Linux 专有 send flag）：非 Linux POSIX 平台
 * 定义为 0，避免编译失败。等价语义由 DAEMON_SETUP_SIGNALS 的
 * signal(SIGPIPE, SIG_IGN) 保证——peer 关闭时 send 返回 EPIPE 而非触发
 * SIGPIPE 杀进程（对齐 daemons/common platform_compat.c 的兜底做法）。 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("llm.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_llm"
#define DEFAULT_TCP_PORT 8080
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define MAX_THREADS 8
#define MAX_MESSAGES_PER_REQUEST 128

#define LLM_MAX_RETRIES 3
#define LLM_BASE_DELAY_MS 100

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
#define LLM_TIME_CTX_CAP 256
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

/* P0.18.1: generate the common globals (g_running_llm_d etc.), signal
 * handling (signal_handler_llm_d, svc_log_toggle_handler_llm_d),
 * print_usage_llm_d, daemon_handle_client_llm_d, daemon_on_client_llm_d
 * boilerplate, eliminating hand-written duplication. */
DAEMON_DECLARE_COMMON(llm_d, llm, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(llm_d)

static llm_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_threads;
    int max_clients;
} llm_daemon_config_t;

static llm_daemon_config_t g_config = {0};

typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
    char *tools_json;
} request_context_t;

/**
 * @brief Create the request context
 */
static request_context_t *request_context_create(void)
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
static void request_context_destroy(request_context_t *ctx)
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

static int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg)
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

static char *handle_complete(cJSON *params, int id);
static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd);

static char *handle_list_models(cJSON *params __attribute__((unused)), int id)
{
    if (!g_service)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service not ready", id);

    char *json = llm_service_list_models(g_service);
    if (!json)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);

    cJSON *root = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!root)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid list result", id);

    /* Ownership semantics: jsonrpc_build_success attaches root to the
     * response object via cJSON_AddItemToObject and cJSON_Delete frees it
     * recursively; it must NOT be deleted here (otherwise double-free) */
    return jsonrpc_build_success(root, id);
}

static void on_list_models_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_list_models(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Handle the embeddings method
 *
 * Forwards the OpenAI-format request ({"model":..,"input":..}) verbatim to
 * the owning provider's $api_base/embeddings and returns the upstream JSON.
 */
static char *handle_embeddings(cJSON *params, int id)
{
    if (!params || !g_service)
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);

    char *model = NULL;
    cJSON *m = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = AIRY_STRDUP(m->valuestring);

    char *body = cJSON_PrintUnformatted(params);
    if (!body) {
        AIRY_FREE(model);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    char *out = NULL;
    int rc = llm_service_embeddings(g_service, model, body, &out);
    AIRY_FREE(body);
    AIRY_FREE(model);

    if (rc != 0 || !out)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Embedding failed", id);

    cJSON *result = cJSON_Parse(out);
    AIRY_FREE(out);
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid embedding response", id);

    return jsonrpc_build_success(result, id);
}

static void on_embeddings_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_embeddings(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static const char *llm_encoding_for_model(const char *model)
{
    if (!model || model[0] == '\0')
        return "cl100k_base";
    if (strstr(model, "claude"))
        return "claude";
    if (strstr(model, "gpt-3.5") || strstr(model, "text-davinci") || strstr(model, "code-davinci"))
        return "p50k_base";
    return "cl100k_base";
}

static char *handle_count_tokens(cJSON *params, int id)
{
    cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Missing text string", id);
    }

    const char *model = NULL;
    cJSON *model_json = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(model_json) && model_json->valuestring)
        model = model_json->valuestring;
    else if (g_service)
        model = llm_service_default_model(g_service);

    const char *encoding = llm_encoding_for_model(model);
    token_counter_t *counter = token_counter_create(encoding);
    if (!counter)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Token counter unavailable", id);

    size_t tokens = token_counter_count(counter, text->valuestring);
    token_counter_destroy(counter);
    if (tokens == (size_t)-1)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Token counting failed", id);

    cJSON *result = cJSON_CreateObject();
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    cJSON_AddStringToObject(result, "model", model ? model : "default");
    cJSON_AddStringToObject(result, "text", text->valuestring);
    cJSON_AddNumberToObject(result, "tokens", (double)tokens);
    cJSON_AddStringToObject(result, "encoding", encoding);
    return jsonrpc_build_success(result, id);
}

static void on_count_tokens_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_count_tokens(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static char *handle_health_check(cJSON *params __attribute__((unused)), int id)
{
    cJSON *result = cJSON_CreateObject();
    if (!result)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    cJSON_AddStringToObject(result, "service", "llm_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    return jsonrpc_build_success(result, id);
}

static void on_health_check_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_health_check(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

static char *handle_get_stats(cJSON *params __attribute__((unused)), int id)
{
    if (!g_service)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service not ready", id);

    char *json = NULL;
    if (llm_service_stats(g_service, &json) != 0 || !json)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Get stats failed", id);

    cJSON *root = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!root)
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid stats result", id);

    /* Ownership semantics: jsonrpc_build_success attaches root to the
     * response object via cJSON_AddItemToObject and cJSON_Delete frees it
     * recursively; it must NOT be deleted here */
    return jsonrpc_build_success(root, id);
}

static void on_get_stats_method(cJSON *params, int id, void *user_data)
{
    char *response = handle_get_stats(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Wrapper for the complete method (adapts the method_dispatcher
 *        interface)
 */
static void on_complete_method(cJSON *params, int id, void *user_data __attribute__((unused)))
{
    char *response = handle_complete(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        size_t resp_len = strlen(response);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send start fd=%d resp_len=%zu", (int)client_fd,
                          resp_len);
        ssize_t sent = airy_sock_send(client_fd, response, resp_len);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send done fd=%d sent=%zd errno=%d", (int)client_fd,
                          sent, errno);
        AIRY_FREE(response);
    }
}

/**
 * @brief Wrapper for the complete_stream method
 */
static void on_complete_stream_method(cJSON *params, int id,
                                      void *user_data __attribute__((unused)))
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    char *response = handle_complete_stream(params, id, client_fd);
    if (response) {
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/**
 * @brief Handle the complete method
 */
static char *handle_complete(cJSON *params, int id)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    uint64_t start_time = airy_time_ms();

    llm_response_t *resp = NULL;
    int ret = -1;

    for (int attempt = 0; attempt <= LLM_MAX_RETRIES; attempt++) {
        ret = llm_service_complete(g_service, &cfg, &resp);

        if (ret == 0)
            break;

        if (attempt < LLM_MAX_RETRIES) {
            unsigned delay_ms = LLM_BASE_DELAY_MS * (1U << (attempt > 15 ? 15 : attempt));
            SVC_LOG_WARN("LLM complete attempt %d/%d failed (err=%d), retrying in %ums",
                         attempt + 1, LLM_MAX_RETRIES + 1, ret, delay_ms);
            airy_sleep_ms(delay_ms);
        }
    }

    uint64_t end_time = airy_time_ms();

    if (ret != 0) {
        SVC_LOG_ERROR("LLM complete failed after %d attempts (total %llums)", LLM_MAX_RETRIES + 1,
                      (unsigned long long)(end_time - start_time));
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "LLM service unavailable after retries",
                                   id);
    }

    char *resp_json = response_to_json(resp);
    llm_response_free(resp);
    AIRY_FREE((void *)cfg.model);

    if (!resp_json) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Failed to serialize response", id);
    }

    CJSON_PARSE_GUARD(result, resp_json, {
        AIRY_FREE(resp_json);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid response format", id);
    });
    AIRY_FREE(resp_json);

    char *success = jsonrpc_build_success(result, id);
    /* P0.20.8 fix: jsonrpc_build_success transfers result ownership to root
     * via cJSON_AddItemToObject; cJSON_Delete(root) frees result recursively.
     * Must set NULL to prevent CJSON_AUTO_FREE cleanup from double-freeing
     * via cJSON_Delete(result). */
    result = NULL;

    request_context_destroy(ctx);
    return success;
}

/**
 * @brief Handle the complete_stream method
 */
typedef struct {
    airy_sock_t fd;
} llm_stream_ctx_t;

#ifndef _WIN32
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#endif

/**
 * @brief Blocking full send (streaming only)
 *
 * llm_d's streaming callback is driven by the curl write callback: the LLM
 * push rate can exceed the peer's (gateway SSE pull) consumption rate.
 * airy_sock_send uses MSG_DONTWAIT and returns EAGAIN when the buffer is full,
 * making the curl write callback fail -> STREAM-FAIL and the stream breaks.
 *
 * This switches to blocking mode: send loop + poll(POLLOUT) waiting for
 * writability, guaranteeing each incremental chunk is fully delivered before
 * returning. On peer close (EPIPE/ECONNRESET), silently give up without
 * triggering a stream failure.
 */
static void llm_stream_send_all(airy_sock_t fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int sent = send(fd, buf + off, (int)(len - off), 0);
#else
        ssize_t sent = send(fd, buf + off, len - off, MSG_NOSIGNAL);
#endif
        if (sent > 0) {
            off += (size_t)sent;
            continue;
        }
        if (sent < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEINTR)
                break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {

                struct pollfd pfd = {.fd = (int)fd, .events = POLLOUT};
                if (poll(&pfd, 1, 1000) <= 0)
                    break;
                continue;
            }
            if (errno == EINTR)
                continue;
            break;
#endif
        } else {
            break;
        }
    }
}

static void llm_stream_callback(const char *chunk, void *user_data)
{
    llm_stream_ctx_t *sctx = (llm_stream_ctx_t *)user_data;
    llm_stream_send_all(sctx->fd, chunk, strlen(chunk));
}

static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    cfg.stream = 1;

    llm_stream_ctx_t stream_ctx = {.fd = client_fd};

    llm_response_t *resp = NULL;
    int ret = llm_service_complete_stream(g_service, &cfg, llm_stream_callback, &stream_ctx, &resp);

    if (ret != 0) {
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service error", id);
    }

    if (resp) {
        /* 2.1.1.5 修复：流式结束发送 usage 控制帧（RS 'U' body RS），
         * 携带真实 token 消耗与计费金额——此前流式路径完全不回传 usage，
         * IPC 客户端的流式 token 统计与计费恒为 0。adapter 侧按帧协议
         * 解析；cost_usd 由 update_cost_tracking 回填（该帧同步带上，
         * 使 gateway SSE 透传的 usage 事件包含真实费用）。 */
        char usage_frame[256];
        int ufn = snprintf(usage_frame, sizeof(usage_frame),
                           "\x1eU{\"prompt_tokens\":%u,\"completion_tokens\":%u,"
                           "\"total_tokens\":%u,\"cost_usd\":%.6f}\x1e",
                           resp->prompt_tokens, resp->completion_tokens, resp->total_tokens,
                           resp->cost_usd);
        if (ufn > 0)
            llm_stream_send_all(client_fd, usage_frame, (size_t)ufn);
        llm_response_free(resp);
    }

    AIRY_FREE((void *)cfg.model);
    request_context_destroy(ctx);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/**
 * @brief Load the daemon config
 */
static int load_daemon_config(const char *config_path)
{

    g_config.use_tcp = 0;
    g_config.max_threads = MAX_THREADS;
    g_config.max_clients = MAX_CLIENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);

            char *content = (char *)AIRY_MALLOC(len + 1);
            if (content) {
                size_t nread = fread(content, 1, len, f);
                if (nread == (size_t)len) {
                    content[len] = '\0';

                    /* P0.18.2: CJSON_PARSE_GUARD replaces cJSON_Parse + if
                     * (root) + manual cJSON_Delete, using do { ... } while (0)
                     * + break to preserve the original if (root) block
                     * semantics: skip config extraction on parse failure */
                    do {
                        CJSON_PARSE_GUARD(root, content, { break; });
                        cJSON *daemon = cJSON_GetObjectItem(root, "daemon");
                        if (daemon) {
                            cJSON *socket_path = cJSON_GetObjectItem(daemon, "socket_path");
                            if (cJSON_IsString(socket_path)) {
                                AIRY_FREE(g_config.socket_path);
                                g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                            }

                            cJSON *tcp_port = cJSON_GetObjectItem(daemon, "tcp_port");
                            if (cJSON_IsNumber(tcp_port)) {
                                g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                g_config.use_tcp = 1;
                            }

                            cJSON *max_threads = cJSON_GetObjectItem(daemon, "max_threads");
                            if (cJSON_IsNumber(max_threads)) {
                                g_config.max_threads = max_threads->valueint;
                            }
                        }

                    } while (0);
                }
                AIRY_FREE(content);
            }
            fclose(f);
        }
    }

    return 0;
}

/**
 * @brief Release the config resources
 */
static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

static void destroy_service_llm_d(void)
{
    if (g_service) {
        llm_service_destroy(g_service);
        g_service = NULL;
    }
    free_daemon_config();
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_llm_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    /* Model SSoT fallback: when no --manager is given (e.g. started by the
     * CLI /daemon command or a plain exec), load $AIRY_CONFIG_DIR/model.yaml
     * so the provider registry / llm_router always see the configured
     * endpoints. Same pattern as think_d / gateway_d reading model.yaml from
     * airy_config_dir(). Kept in a static buffer for the process lifetime. */
    if (!config_path) {
        static char default_model_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir) {
            int plen = snprintf(default_model_path, sizeof(default_model_path), "%s/model.yaml",
                                cfg_dir);
            if (plen > 0 && plen < (int)sizeof(default_model_path))
                config_path = default_model_path;
        }
    }

    airy_sock_init();
    airy_mtx_init(&g_running_lock_llm_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_llm_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(llm_d);
#endif

    /* Keep the initial log level WARN (SIGUSR1 toggles between DEBUG/INFO,
     * see the generated svc_log_toggle_handler_llm_d). Debugging:
     * AIRY_LLM_D_DEBUG=1 outputs DEBUG-level logs */
    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_LLM_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1') ? (log_level_t)LOG_LEVEL_DEBUG :
                                             (log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    daemon_cupolas_init("llm_d");

    load_daemon_config(config_path);
    use_tcp = use_tcp || g_config.use_tcp;

    SVC_LOG_INFO("LLM service starting, manager=%s", config_path ? config_path : "default");

    g_service = llm_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create service");
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    int tcp_port = g_config.tcp_port ? (int)g_config.tcp_port : DEFAULT_TCP_PORT;
    const char *unix_path = g_config.socket_path ? g_config.socket_path : DEFAULT_SOCKET_PATH_UNIX;
    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, tcp_port, unix_path, DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    if (use_tcp)
        SVC_LOG_INFO("Listening on TCP port %d", tcp_port);
    else
        SVC_LOG_INFO("Listening on %s", unix_path);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = g_config.max_threads > 0 ? g_config.max_threads : 4;
    ev_config.thread_pool_max = g_config.max_threads > 0 ? g_config.max_threads : 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_llm_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : unix_path;
    int ret = daemon_init_event_driver("llm_d", "llm", sock_addr, use_tcp ? tcp_port : 0, "ai,core",
                                       use_tcp, &ev_config, &g_event_driver_llm_d, &g_bsd_llm_d,
                                       &g_bipc_llm_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_llm_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_llm_d = daemon_event_driver_get_dispatcher(g_event_driver_llm_d);
    method_dispatcher_register(g_dispatcher_llm_d, "complete", on_complete_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "complete_stream", on_complete_stream_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "list_models", on_list_models_method, NULL);
    /* Standard L2 protocol methods (02-l2-service-protocol.md:
     * llm.count_tokens / llm.health_check / llm.get_stats)
     */
    method_dispatcher_register(g_dispatcher_llm_d, "count_tokens", on_count_tokens_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "health_check", on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "embeddings", on_embeddings_method, NULL);

    method_dispatcher_register(g_dispatcher_llm_d, "shutdown", on_shutdown_method_llm_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (llm.* namespace)", 8);

    if (daemon_event_driver_add_server_fd(g_event_driver_llm_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_llm_d);
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("LLM service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_llm_d);

    daemon_cleanup_standard(g_bipc_llm_d, g_bsd_llm_d, g_event_driver_llm_d, server_fd, unix_path,
                            destroy_service_llm_d, &g_running_lock_llm_d);

    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
