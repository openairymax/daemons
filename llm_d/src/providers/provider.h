/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file provider.h
 * @brief 提供商适配器接口定义
 */

#ifndef AIRY_RT_LLM_PROVIDER_H
#define AIRY_RT_LLM_PROVIDER_H

#include "llm_service.h"

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct provider_ctx provider_ctx_t;


typedef struct {
    char api_key[256];
    char api_key_env[128]; /* 记录 model.yaml api_key_env 名（如 DEEPSEEK_API_KEY），
                             * 用于请求时从 secrets.env 热加载（启动后填 key 无需重启） */
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

/* 热加载：每次请求前调用，若当前 api_key 为空则从 $AIRY_HOME/config/secrets.env
 * 读取 base_ctx->api_key_env 对应的值填充（启动后填 key 无需重启）。 */
void provider_refresh_api_key(provider_base_ctx_t *base_ctx);

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code);

void provider_http_resp_free(provider_http_resp_t *resp);

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model);

int provider_parse_openai_response(const char *body, llm_response_t **out);


typedef int (*provider_stream_chunk_cb_t)(const char *data_line, void *user_data);

int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, provider_stream_chunk_cb_t on_chunk,
                              void *chunk_user_data, long *out_http_code);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDER_H */