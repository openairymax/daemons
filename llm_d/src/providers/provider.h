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

int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, provider_stream_chunk_cb_t on_chunk,
                              void *chunk_user_data, long *out_http_code);

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