// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/**
 * @file provider.c
 * @brief 提供商通用工具实现
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 改进说明：
 * 1. 提取公共代码到通用函数
 * 2. 提供 OpenAI 兼容的请求构建和响应解析
 * 3. 统一的 HTTP 请求处理
 */

#include "provider.h"
#include "svc_logger.h"
#include "platform.h"

#include <cjson/cJSON.h>
/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD 宏 */
#include <cjson_helpers.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- secrets.env 热加载 ---------- */

/* 保护 api_key 写入（llm_d 线程池并发请求可能同时触发热加载） */
static airy_mtx_t g_secrets_refresh_lock;
static bool g_secrets_lock_inited = false;

static void secrets_lock_ensure(void)
{
    if (!g_secrets_lock_inited) {
        airy_mtx_init(&g_secrets_refresh_lock);
        g_secrets_lock_inited = true;
    }
}

/**
 * @brief 从 $AIRY_HOME/config/secrets.env 读取指定 key 的值
 * @return 动态分配字符串（调用方 AIRY_FREE），未找到/读取失败返回 NULL
 */
static char *secrets_env_read(const char *key_name)
{
    if (!key_name || !key_name[0])
        return NULL;

    char path[1024];
    snprintf(path, sizeof(path), "%s/secrets.env", airy_config_dir());

    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    char line[1024];
    char *found = NULL;
    while (fgets(line, sizeof(line), f)) {
        /* 跳过空行与注释 */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;

        /* key = 等号前去除首尾空白 */
        char *key_end = eq;
        while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t'))
            key_end--;
        size_t key_len = (size_t)(key_end - p);
        if (key_len == 0 || key_len != strlen(key_name) ||
            strncmp(p, key_name, key_len) != 0)
            continue;

        /* value = 等号后去除首尾空白与行尾换行 */
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t vlen = strlen(v);
        while (vlen > 0 && (v[vlen - 1] == '\n' || v[vlen - 1] == '\r' ||
                            v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
            vlen--;
        if (vlen > 0 && v[0] == '"' && v[vlen - 1] == '"') {
            v++;
            vlen -= 2;
        }
        if (vlen > 0) {
            found = (char *)AIRY_MALLOC(vlen + 1);
            if (found) {
                __builtin_memcpy(found, v, vlen);
                found[vlen] = '\0';
            }
        }
        break;
    }

    explicit_bzero(line, sizeof(line));
    fclose(f);
    return found;
}

/**
 * @brief 请求时热加载 api_key：每次请求重新解析（环境变量优先，secrets.env 兜底）
 *
 * 设计意图：普通用户无需重启 llm_d——在 $AIRY_HOME/config/secrets.env
 * 填入 API key 后保存，下一次请求自动生效；后续修改 key 同样即时生效。
 * 优先级：环境变量（bootstrap 启动时 source 的 secrets.env）> secrets.env 文件。
 * 安全性：key 值不写入日志（仅记录 env 名与来源）；临时缓冲用后清零。
 */
void provider_refresh_api_key(provider_base_ctx_t *base_ctx)
{
    if (!base_ctx || base_ctx->api_key_env[0] == '\0')
        return;

    /* 环境变量优先（bootstrap 启动时 source 的 secrets.env） */
    const char *env_val = getenv(base_ctx->api_key_env);
    const char *new_key = (env_val && env_val[0]) ? env_val : NULL;
    char *file_val = NULL;
    if (!new_key) {
        file_val = secrets_env_read(base_ctx->api_key_env);
        new_key = file_val;
    }
    if (!new_key || !new_key[0])
        return;

    secrets_lock_ensure();
    airy_mtx_lock(&g_secrets_refresh_lock);
    /* 仅当值发生变化才更新并记录日志，避免刷屏 */
    if (strcmp(base_ctx->api_key, new_key) != 0) {
        size_t n = strlen(new_key);
        if (n < sizeof(base_ctx->api_key)) {
            explicit_bzero(base_ctx->api_key, sizeof(base_ctx->api_key));
            __builtin_memcpy(base_ctx->api_key, new_key, n + 1);
            SVC_LOG_INFO("C-L02: PROVIDER: KEY-RELOAD api_key_env=%s source=%s",
                         base_ctx->api_key_env, env_val ? "env" : "secrets.env");
        }
    }
    airy_mtx_unlock(&g_secrets_refresh_lock);

    if (file_val) {
        explicit_bzero(file_val, strlen(file_val));
        AIRY_FREE(file_val);
    }
}

/* ---------- 通用上下文初始化 ---------- */

static const char *resolve_api_key(const char *api_key)
{
    if (!api_key || api_key[0] == '\0') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (strncmp(api_key, "env:", 4) == 0) {
        const char *env_name = api_key + 4;
        const char *env_val = getenv(env_name);
        if (env_val && env_val[0]) {
            return env_val;
        }
        SVC_LOG_WARN("Environment variable '%s' not set or empty", env_name);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
    }

    return api_key;
}

static const char *fallback_env_key(const char *provider_name)
{
    if (!provider_name) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (strcmp(provider_name, "openai") == 0)
        return getenv("OPENAI_API_KEY");
    if (strcmp(provider_name, "anthropic") == 0)
        return getenv("ANTHROPIC_API_KEY");
    if (strcmp(provider_name, "deepseek") == 0)
        return getenv("DEEPSEEK_API_KEY");
    if (strcmp(provider_name, "google") == 0)
        return getenv("GOOGLE_AI_API_KEY");
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

static const char *guess_provider_from_url(const char *url)
{
    if (!url) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (strstr(url, "openai.com"))
        return "openai";
    if (strstr(url, "anthropic.com"))
        return "anthropic";
    if (strstr(url, "deepseek.com"))
        return "deepseek";
    if (strstr(url, "googleapis.com"))
        return "google";
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

void provider_base_init(provider_base_ctx_t *base_ctx, const char *api_key, const char *api_base,
                        const char *organization, double timeout_sec, int max_retries,
                        const char *default_base)
{
    if (!base_ctx)
        return;

    __builtin_memset(base_ctx, 0, sizeof(provider_base_ctx_t));

    /* 记录 api_key_env 名（供请求时 secrets.env 热加载）。
     * 优先从 "env:NAME" 前缀提取；否则按 provider/base_url 推断标准环境变量名。 */
    base_ctx->api_key_env[0] = '\0';
    if (api_key && strncmp(api_key, "env:", 4) == 0) {
        const char *env_name = api_key + 4;
        size_t env_len = strlen(env_name);
        if (env_len > 0 && env_len < sizeof(base_ctx->api_key_env)) {
            __builtin_memcpy(base_ctx->api_key_env, env_name, env_len + 1);
        }
    } else {
        const char *guessed = guess_provider_from_url(api_base ? api_base : default_base);
        if (guessed) {
            const char *env_name = NULL;
            if (strcmp(guessed, "openai") == 0)
                env_name = "OPENAI_API_KEY";
            else if (strcmp(guessed, "anthropic") == 0)
                env_name = "ANTHROPIC_API_KEY";
            else if (strcmp(guessed, "deepseek") == 0)
                env_name = "DEEPSEEK_API_KEY";
            else if (strcmp(guessed, "google") == 0)
                env_name = "GOOGLE_AI_API_KEY";
            if (env_name)
                AIRY_STRNCPY_TERM(base_ctx->api_key_env, env_name, sizeof(base_ctx->api_key_env));
        }
    }

    const char *resolved_key = resolve_api_key(api_key);
    if (!resolved_key || resolved_key[0] == '\0') {
        const char *guessed = guess_provider_from_url(api_base ? api_base : default_base);
        resolved_key = fallback_env_key(guessed);
    }

    if (resolved_key) {
        size_t key_len = strlen(resolved_key);
        if (key_len < sizeof(base_ctx->api_key)) {
            __builtin_memcpy(base_ctx->api_key, resolved_key, key_len + 1);
        }
    }

    if (api_base) {
        size_t base_len = strlen(api_base);
        if (base_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, api_base, base_len + 1);
        }
    } else if (default_base) {
        size_t default_len = strlen(default_base);
        if (default_len < sizeof(base_ctx->api_base)) {
            __builtin_memcpy(base_ctx->api_base, default_base, default_len + 1);
        }
    }

    if (organization) {
        size_t org_len = strlen(organization);
        if (org_len < sizeof(base_ctx->organization)) {
            __builtin_memcpy(base_ctx->organization, organization, org_len + 1);
        }
    }

    base_ctx->timeout_sec = timeout_sec > 0 ? timeout_sec : 120.0;
    base_ctx->max_retries = max_retries > 0 ? max_retries : 3;

    SVC_LOG_INFO("C-L02: PROVIDER: BASE-INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 base_ctx->api_base[0] ? base_ctx->api_base : "(none)", base_ctx->timeout_sec,
                 base_ctx->max_retries, base_ctx->api_key[0] ? 1 : 0);
}

/* ---------- HTTP 响应管理 ---------- */

void provider_http_resp_free(provider_http_resp_t *resp)
{
    if (resp) {
        AIRY_FREE(resp->data);
        AIRY_FREE(resp);
    }
}

/* ---------- HTTP 回调 ---------- */

static size_t http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    provider_http_resp_t *mem = (provider_http_resp_t *)userp;

    size_t new_size = mem->size + realsize + 1;
    if (new_size > mem->capacity) {
        size_t new_cap = mem->capacity * 2;
        if (new_cap < new_size)
            new_cap = new_size;

        char *ptr = (char *)AIRY_REALLOC(mem->data, new_cap);
        if (!ptr)
            return 0;

        mem->data = ptr;
        mem->capacity = new_cap;
    }

    __builtin_memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

/* ---------- HTTP POST 实现 ---------- */

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code)
{
    if (!url || !body || !out_response || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    provider_http_resp_t *resp =
        (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(provider_http_resp_t));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    CURL *curl = NULL;
    int retry = 0;
    int success = -1;
    CURLcode res;
    long http_code = 0;

    while (retry <= max_retries) {
        curl = curl_easy_init();
        if (!curl) {
            SVC_LOG_ERROR("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d/%d "
                          "STACK: provider_http_post curl_easy_init",
                          url, errno, retry, max_retries);
            provider_http_resp_free(resp);
            return AIRY_ERR_UNKNOWN;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            success = 0;
            curl_easy_cleanup(curl);
            break;
        }

        SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d/%d "
                     "curl_error=%s",
                     url, errno, retry + 1, max_retries, curl_easy_strerror(res));
        retry++;
        curl_easy_cleanup(curl);
        if (retry <= max_retries) {
            AIRY_FREE(resp->data);
            resp->data = NULL;
            resp->size = 0;
            resp->capacity = 0;
        }
    }

    if (success != 0) {
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    *out_response = resp;
    *out_http_code = http_code;
    return AIRY_OK;
}

/* ---------- 通用请求构建 ---------- */

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model)
{
    if (!manager) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("C-L02: PROVIDER: REQUEST-BUILD-FAIL reason=oom_root "
                      "STACK: provider_build_openai_request");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    const char *model = manager->model && manager->model[0] ? manager->model : default_model;
    cJSON_AddStringToObject(root, "model", model ? model : "gpt-3.5-turbo");
    cJSON_AddNumberToObject(root, "temperature",
                            manager->temperature > 0 ? manager->temperature : 0.7);

    if (manager->top_p > 0) {
        cJSON_AddNumberToObject(root, "top_p", manager->top_p);
    }

    if (manager->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", manager->max_tokens);
    }

    if (manager->stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
    }

    if (manager->presence_penalty != 0) {
        cJSON_AddNumberToObject(root, "presence_penalty", manager->presence_penalty);
    }

    if (manager->frequency_penalty != 0) {
        cJSON_AddNumberToObject(root, "frequency_penalty", manager->frequency_penalty);
    }

    if (manager->stop_count > 0 && manager->stop) {
        cJSON *stop = cJSON_CreateArray();
        for (size_t i = 0; i < manager->stop_count; ++i) {
            cJSON_AddItemToArray(stop, cJSON_CreateString(manager->stop[i]));
        }
        cJSON_AddItemToObject(root, "stop", stop);
    }

    /* Function calling：透传 tools 定义（OpenAI tools 数组 JSON 字符串） */
    if (manager->tools_json && manager->tools_json[0]) {
        CJSON_PARSE_GUARD(tools, manager->tools_json, {});
        if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0) {
            cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));
        }
    }

    cJSON *msgs = cJSON_CreateArray();
    for (size_t i = 0; i < manager->message_count; ++i) {
        cJSON *msg = cJSON_CreateObject();
        const char *role = manager->messages[i].role ? manager->messages[i].role : "user";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        cJSON_AddStringToObject(msg, "role", role);
        cJSON_AddStringToObject(msg, "content", content);
        /* role="tool"：携带 tool_call_id */
        if (manager->messages[i].tool_call_id && manager->messages[i].tool_call_id[0]) {
            cJSON_AddStringToObject(msg, "tool_call_id", manager->messages[i].tool_call_id);
        }
        /* role="assistant"：携带 tool_calls（OpenAI 格式 JSON 数组字符串） */
        if (manager->messages[i].tool_calls_json && manager->messages[i].tool_calls_json[0]) {
            CJSON_PARSE_GUARD(tc, manager->messages[i].tool_calls_json, {});
            if (cJSON_IsArray(tc)) {
                cJSON_AddItemToObject(msg, "tool_calls", cJSON_Duplicate(tc, 1));
            }
        }
        cJSON_AddItemToArray(msgs, msg);
    }
    cJSON_AddItemToObject(root, "messages", msgs);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ---------- 通用响应解析 ---------- */

int provider_parse_openai_response(const char *body, llm_response_t **out)
{
    if (!body || !out) {
        return AIRY_ERR_INVALID_PARAM;
    }

    /* P0.18.2: CJSON_PARSE_GUARD 替代 cJSON_Parse + NULL 检查 + 手动 cJSON_Delete */
    CJSON_PARSE_GUARD(root, body, {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=cjson_parse_error "
                      "STACK: provider_parse_openai_response");
        return AIRY_ERR_PARSE_ERROR;
    });

    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_resp "
                      "STACK: provider_parse_openai_response");
        /* root 由 CJSON_AUTO_FREE 自动释放 */
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        resp->id = AIRY_STRDUP(id->valuestring);
    }

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model) && model->valuestring) {
        resp->model = AIRY_STRDUP(model->valuestring);
    }

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created)) {
        resp->created = (uint64_t)created->valuedouble;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        int size = cJSON_GetArraySize(choices);
        resp->choice_count = (size_t)size;
        resp->choices = (llm_message_t *)AIRY_CALLOC((size_t)size, sizeof(llm_message_t));

        if (!resp->choices) {
            SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_choices "
                          "STACK: provider_parse_openai_response");
            /* root 由 CJSON_AUTO_FREE 自动释放 */
            llm_response_free(resp);
            return AIRY_ERR_OUT_OF_MEMORY;
        }

        for (int i = 0; i < size; ++i) {
            cJSON *choice = cJSON_GetArrayItem(choices, i);
            cJSON *message = cJSON_GetObjectItem(choice, "message");
            if (message) {
                cJSON *role = cJSON_GetObjectItem(message, "role");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsString(role) && role->valuestring) {
                    resp->choices[i].role = AIRY_STRDUP(role->valuestring);
                }
                if (cJSON_IsString(content) && content->valuestring) {
                    resp->choices[i].content = AIRY_STRDUP(content->valuestring);
                }
                /* Function calling：提取 tool_calls（原始 JSON 数组，序列化为字符串） */
                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
                    char *tc_json = cJSON_PrintUnformatted(tool_calls);
                    if (tc_json) {
                        resp->choices[i].tool_calls_json = tc_json;
                    }
                }
            }
            cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
            if (cJSON_IsString(finish) && finish->valuestring && !resp->finish_reason) {
                resp->finish_reason = AIRY_STRDUP(finish->valuestring);
            }
        }
    }

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON *prompt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *completion = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *total = cJSON_GetObjectItem(usage, "total_tokens");
        if (cJSON_IsNumber(prompt))
            resp->prompt_tokens = (uint32_t)prompt->valuedouble;
        if (cJSON_IsNumber(completion))
            resp->completion_tokens = (uint32_t)completion->valuedouble;
        if (cJSON_IsNumber(total))
            resp->total_tokens = (uint32_t)total->valuedouble;
    }

    /* root 由 CJSON_AUTO_FREE 自动释放 */
    *out = resp;
    return AIRY_OK;
}

/* ========== SSE 流式传输实现 ========== */

typedef struct {
    char *line_buf;
    size_t line_cap;
    size_t line_len;
    provider_stream_chunk_cb_t on_chunk;
    void *chunk_user_data;
    int cancelled; /**< 流被取消（on_chunk 返回错误） */
    int done;      /**< 收到 [DONE]：正常流结束标记 */
} sse_stream_ctx_t;

static void sse_ctx_init(sse_stream_ctx_t *sse, provider_stream_chunk_cb_t cb, void *user_data)
{
    __builtin_memset(sse, 0, sizeof(*sse));
    sse->line_cap = 4096;
    sse->line_buf = (char *)AIRY_MALLOC(sse->line_cap);
    if (!sse->line_buf) {
        SVC_LOG_ERROR("C-L02: PROVIDER: SSE-INIT-FAIL reason=oom cap=%zu "
                      "STACK: sse_ctx_init",
                      sse->line_cap);
    }
    sse->on_chunk = cb;
    sse->chunk_user_data = user_data;
}

static void sse_ctx_destroy(sse_stream_ctx_t *sse)
{
    if (sse) {
        AIRY_FREE(sse->line_buf);
        sse->line_buf = NULL;
    }
}

static int sse_feed_line(sse_stream_ctx_t *sse, const char *line, size_t len)
{
    if (!line || len == 0)
        return 0;

    if (len >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *data_start = line + 5;
        while (*data_start == ' ' || *data_start == '\t')
            data_start++;
        size_t data_len = len - (size_t)(data_start - line);

        if (data_len >= 6 && memcmp(data_start, "[DONE]", 6) == 0) {
            /* 正常流结束：[DONE] 是 SSE 协议的标准收尾，不是错误。
             * 仅置 done 标记；cancelled 留给 on_chunk 错误取消使用，
             * 否则 curl 写回调因 cancelled 返回 0 会误报 CURLE_WRITE_ERROR
             * 并把正常完成当成 STREAM-FAIL（历史缺陷，真实流式调用方暴露）。 */
            sse->done = 1;
            return 0;
        }

        if (sse->on_chunk) {
            char *tmp = (char *)AIRY_MALLOC(data_len + 1);
            if (tmp) {
                __builtin_memcpy(tmp, data_start, data_len);
                tmp[data_len] = '\0';
                int ret = sse->on_chunk(tmp, sse->chunk_user_data);
                AIRY_FREE(tmp);
                if (ret != 0) {
                    sse->cancelled = 1;
                    return ret;
                }
            }
        }
    }

    return 0;
}

static void sse_process_buffer(sse_stream_ctx_t *sse)
{
    if (sse->line_len == 0)
        return;

    char *p = sse->line_buf;
    char *end = p + sse->line_len;

    while (p < end) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            break;

        size_t line_len = (size_t)(nl - p);
        if (line_len > 0 && *(nl - 1) == '\r')
            line_len--;

        if (line_len > 0) {
            int r = sse_feed_line(sse, p, line_len);
            if (r != 0 || sse->cancelled)
                return;
        }

        p = nl + 1;
    }

    if (p < end) {
        size_t remaining = (size_t)(end - p);
        __builtin_memmove(sse->line_buf, p, remaining);
        sse->line_len = remaining;
    } else {
        sse->line_len = 0;
    }
}

static size_t sse_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    sse_stream_ctx_t *sse = (sse_stream_ctx_t *)userp;

    if (sse->cancelled)
        return 0;

    size_t needed = sse->line_len + realsize + 1;
    if (needed > sse->line_cap) {
        size_t new_cap = sse->line_cap * 2;
        while (new_cap < needed)
            new_cap *= 2;
        char *ptr = (char *)AIRY_REALLOC(sse->line_buf, new_cap);
        if (!ptr)
            return 0;
        sse->line_buf = ptr;
        sse->line_cap = new_cap;
    }

    __builtin_memcpy(sse->line_buf + sse->line_len, contents, realsize);
    sse->line_len += realsize;
    sse->line_buf[sse->line_len] = '\0';

    sse_process_buffer(sse);

    if (sse->cancelled)
        return 0;
    return realsize;
}

int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, provider_stream_chunk_cb_t on_chunk,
                              void *chunk_user_data, long *out_http_code)
{
    if (!url || !body || !on_chunk || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    sse_stream_ctx_t sse;
    sse_ctx_init(&sse, on_chunk, chunk_user_data);
    if (!sse.line_buf) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        sse_ctx_destroy(&sse);
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d "
                      "STACK: provider_http_post_stream curl_easy_init",
                      url, errno);
        return AIRY_ERR_UNKNOWN;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    *out_http_code = http_code;
    curl_easy_cleanup(curl);

    if (sse.line_len > 0) {
        sse_process_buffer(&sse);
    }

    sse_ctx_destroy(&sse);

    if (res != CURLE_OK) {
        SVC_LOG_WARN("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d curl_error=%s", url, errno,
                 curl_easy_strerror(res));
        return AIRY_ERR_IO;
    }

    return AIRY_OK;
}
