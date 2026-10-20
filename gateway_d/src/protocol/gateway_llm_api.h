/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

#ifndef AIRY_RT_GATEWAY_LLM_API_H
#define AIRY_RT_GATEWAY_LLM_API_H

#include "gateway_proto_types.h"

#include "airy_abi.h"

AIRY_ABI_BEGIN

typedef struct gw_llm_api gw_llm_api_t;

typedef struct {
    char default_model[128]; /* 数据面注入；空串表示未配置，请求缺 model 时 fail-closed */
    uint32_t max_tokens_default;
    double temperature_default;
    double top_p_default;
    uint32_t rate_limit_rpm;
    uint32_t retry_max;
    uint32_t retry_base_ms;
} gw_llm_api_config_t;

#define GW_LLM_API_CONFIG_DEFAULTS  \
    {.default_model = "",           \
     .max_tokens_default = 4096,    \
     .temperature_default = 0.7,    \
     .top_p_default = 1.0,          \
     .rate_limit_rpm = 60,          \
     .retry_max = 3,                \
     .retry_base_ms = 1000}

typedef int (*gw_llm_call_fn)(const char *model, const char *messages_json,
                              const char *functions_json, double temperature, int max_tokens,
                              char **response_json, void *user_data);

typedef int (*gw_llm_embed_fn)(const char *model, const char *input_json, char **response_json,
                               void *user_data);

gw_llm_api_t *gw_llm_api_create(const gw_llm_api_config_t *config);
void gw_llm_api_destroy(gw_llm_api_t *api);

int gw_llm_api_init(gw_llm_api_t *api);
int gw_llm_api_shutdown(gw_llm_api_t *api);

int gw_llm_api_set_llm_call(gw_llm_api_t *api, gw_llm_call_fn fn, void *user_data);

int gw_llm_api_set_embed_fn(gw_llm_api_t *api, gw_llm_embed_fn fn, void *user_data);

gw_proto_request_handler_t gw_llm_api_get_handler(gw_llm_api_t *api);
void *gw_llm_api_get_handler_data(gw_llm_api_t *api);

AIRY_ABI_END

#endif
