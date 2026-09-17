/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file provider.h
 * @brief Provider adapter interface definitions.
 */

#ifndef LLM_D_PROVIDERS_PROVIDER_H
#define LLM_D_PROVIDERS_PROVIDER_H

#include "llm_service.h"

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct provider_ctx provider_ctx_t;


typedef struct {
    char api_key[256];
    char api_key_env[128]; /* model.yaml api_key_env name (e.g. DEEPSEEK_API_KEY);
                            * hot-loaded from secrets.env per request, so keys
                            * filled after startup need no restart */
    char api_base[512];
    char organization[128];
    double timeout_sec;
    int max_retries;
} provider_base_ctx_t;


typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} provider_http_resp_t;


typedef struct {
    const char *name;
    const char *default_model;
    const char *default_base_url;
    provider_ctx_t *(*init)(const char *name, const char *api_key, const char *api_base,
                            const char *organization, double timeout_sec, int max_retries);
    void (*destroy)(provider_ctx_t *ctx);
    int (*complete)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                    llm_response_t **out_response);
    int (*complete_stream)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                           llm_stream_callback_t callback, void *callback_data,
                           llm_response_t **out_response);
} provider_ops_t;


typedef struct {
    const char *name;
    const provider_ops_t *ops;
    provider_ctx_t *ctx;
    char **models;
    /* 与 models 同下标对齐的每模型输出上限（0 = 未配置）：registry 装配时
     * 从 provider_config_t 复制，供生成参数解析查询。 */
    int *model_max_output;
} provider_t;


void provider_base_init(provider_base_ctx_t *base_ctx, const char *api_key, const char *api_base,
                        const char *organization, double timeout_sec, int max_retries,
                        const char *default_base);

/* 获取 provider 的 base 上下文（api_base/api_key/timeout_sec 等）。
 * 约定：所有 provider 的 ctx 首字段均为 provider_base_ctx_t base。 */
provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx);

/* Hot reload: called before each request; if the current api_key is empty,
 * fills it from $AIRY_HOME/config/secrets.env using base_ctx->api_key_env
 * (keys filled after startup need no restart). */
void provider_refresh_api_key(provider_base_ctx_t *base_ctx);

/* 出网传输策略唯一实现（SSoT）：连接超时 / 总超时 / 代理 / 自定义 CA /
 * 重定向与证书校验。所有出网调用点（非流式、流式、google、anthropic）
 * 一律经此施加，禁止各自 curl_easy_setopt 副本。
 * timeout_sec 是整次 POST（含全部重试）的墙钟预算，不是单次尝试的预算。 */
void provider_http_setup(CURL *curl, double timeout_sec);

/* 出网失败分诊：把 libcurl 错误码归为可判读类别（DNS/CONNECT/TIMEOUT/TLS/
 * PROXY/NET_IO/OTHER），供日志与用户面诊断使用。返回值恒非 NULL。 */
const char *provider_http_diag(CURLcode code);

/* 出网重试策略唯一实现（SSoT，实现见 provider_http.c）。四者配套使用：
 *   retryable = provider_retryable(错误码)                  —— 是否值得重试
 *   delay     = provider_backoff_ms(已重试次数)             —— 指数退避 + 抖动
 *   left      = provider_left_sec(预算, 起始时刻)           —— 剩余墙钟秒数
 *   ok        = provider_retry_budget_ok(预算, 起点, delay) —— 退避后是否仍够一次
 * 只有四者同时成立才允许重试；流式还须满足"首包未下发"
 * （见 provider_http_post_stream）。任何一处复制这些判断都会造成同构点漂移。 */
#define PROVIDER_RETRY_BASE_MS 200U
#define PROVIDER_RETRY_MAX_MS 2000U
#define PROVIDER_RETRY_JITTER_PCT 25U
/* 退避等待之后仍须剩下的最小尝试窗口：低于此值就放弃重试，避免把调用方的
 * 墙钟预算空转在一次注定超时的尝试上。 */
#define PROVIDER_RETRY_MIN_LEFT_MS 1000U

int provider_retryable(CURLcode code);
uint32_t provider_backoff_ms(int attempt);
double provider_left_sec(double timeout_sec, uint64_t start_ms);
int provider_retry_budget_ok(double timeout_sec, uint64_t start_ms, uint32_t delay_ms);

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code);

void provider_http_resp_free(provider_http_resp_t *resp);

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model);

int provider_parse_openai_response(const char *body, llm_response_t **out);

/* Grow-on-demand string append used by the streaming accumulators (content
 * and reasoning_content). Returns the (possibly reallocated) buffer; on
 * allocation failure returns NULL and leaves the input buffer untouched. */
char *provider_buf_append(char *buf, size_t *cap, size_t *len, const char *text);


typedef int (*provider_stream_chunk_cb_t)(const char *data_line, void *user_data);

/* 流式 POST。重试边界：只有"上游一个字节都没下发"的失败才可重试——一旦写回调
 * 收到过数据，后续分片可能已经经 on_chunk 交付给用户，重试会造成重复输出，故
 * 此时无论错误是否瞬时都直接失败。HTTP 状态码不为 0 表示请求已到达上游，其重试
 * 策略归调用方（429/限流循环），此处不重试。 */
int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, int max_retries,
                              provider_stream_chunk_cb_t on_chunk, void *chunk_user_data,
                              long *out_http_code);

/* 流式控制帧发射（SSoT 唯一实现，收敛 openai/deepseek/local 的同构 static
 * 副本）。帧格式：工具帧 RS 'T' <json> RS；推理帧 RS 'R' <reasoning> RS。
 * RS(0x1E) 不出现在 cJSON 输出与 LLM 文本中，分帧无歧义；各段均 NUL 结尾
 * （llm_stream_callback 对 chunk 调 strlen()）。 */
void provider_emit_tool_frame(llm_stream_callback_t cb, void *ud, const char *tc_json);
void provider_emit_reasoning_frame(llm_stream_callback_t cb, void *ud, const char *reasoning);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_PROVIDER_H */