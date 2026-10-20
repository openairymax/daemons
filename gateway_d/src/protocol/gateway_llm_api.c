// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_llm_api.h"

#include "gateway_jsonpick.h"
#include "airy_memory.h"
#include "platform_sync.h"
#include "sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "error.h"

#include "logging.h"

struct gw_llm_api {
    gw_llm_api_config_t config;
    gw_llm_call_fn llm_call_fn;
    void *llm_call_data;
    gw_llm_embed_fn embed_fn;
    void *embed_data;
    bool initialized;
    bool healthy;
    uint64_t request_count;
    uint64_t error_count;
    uint64_t tokens_used;
    time_t window_start;
    uint32_t window_requests;
    airy_mtx_t lock;
};

static int llm_api_request(const char *method, const char *path, const char *body_json,
                                  char **response_json, void *user_data);

gw_llm_api_t *gw_llm_api_create(const gw_llm_api_config_t *config)
{
    gw_llm_api_t *api = (gw_llm_api_t *)AIRY_CALLOC(1, sizeof(gw_llm_api_t));
    if (!api) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (config) {
        api->config = *config;
    } else {
        gw_llm_api_config_t defaults = GW_LLM_API_CONFIG_DEFAULTS;
        api->config = defaults;
    }
    return api;
}

void gw_llm_api_destroy(gw_llm_api_t *api)
{
    if (!api)
        return;
    if (api->initialized) {
        gw_llm_api_shutdown(api);
    }
    airy_mtx_destroy(&api->lock);
    AIRY_FREE(api);
}

int gw_llm_api_init(gw_llm_api_t *api)
{
    if (!api)
        return AIRY_ERR_INVALID_PARAM;
    if (api->initialized)
        return 0;
    airy_mtx_init(&api->lock);
    api->initialized = true;
    api->healthy = true;
    api->request_count = 0;
    api->error_count = 0;
    api->tokens_used = 0;
    api->window_start = time(NULL);
    api->window_requests = 0;
    return 0;
}

int gw_llm_api_shutdown(gw_llm_api_t *api)
{
    if (!api || !api->initialized)
        return AIRY_ERR_INVALID_PARAM;
    api->initialized = false;
    api->healthy = false;
    return 0;
}

int gw_llm_set_call(gw_llm_api_t *api, gw_llm_call_fn fn, void *user_data)
{
    if (!api || !fn)
        return AIRY_ERR_INVALID_PARAM;
    api->llm_call_fn = fn;
    api->llm_call_data = user_data;
    return 0;
}

int gw_llm_set_embed(gw_llm_api_t *api, gw_llm_embed_fn fn, void *user_data)
{
    if (!api || !fn)
        return AIRY_ERR_INVALID_PARAM;
    api->embed_fn = fn;
    api->embed_data = user_data;
    return 0;
}

static bool check_rate_limit(gw_llm_api_t *api)
{
    bool allowed = false;
    airy_mtx_lock(&api->lock);
    time_t now = time(NULL);
    if (now - api->window_start >= 60) {
        api->window_start = now;
        api->window_requests = 0;
    }
    if (api->window_requests < api->config.rate_limit_rpm) {
        api->window_requests++;
        allowed = true;
    }
    airy_mtx_unlock(&api->lock);
    return allowed;
}

static const char *resolve_model(const gw_llm_api_t *api, const char *requested)
{
    if (requested && requested[0])
        return requested;
    return api->config.default_model[0] ? api->config.default_model : NULL;
}

static int handle_chat_completions(gw_llm_api_t *api, const char *body_json, char **response_json)
{
    if (!api->llm_call_fn) {
        AIRY_LOG_ERROR("no LLM backend configured for chat completions");
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"No LLM backend configured\","
                                     "\"type\":\"server_error\",\"code\":503}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_NULL_POINTER;
    }

    if (!check_rate_limit(api)) {
        AIRY_LOG_WARN("rate limit exceeded: window_requests=%u, limit=%u", api->window_requests,
                      api->config.rate_limit_rpm);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Rate limit exceeded\","
                                     "\"type\":\"rate_limit_error\",\"code\":429}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_OVERFLOW;
    }

    cJSON *root = gw_json_load(body_json);
    char *req_model = gw_json_str(root, "model");
    char *messages = gw_json_raw(root, "messages");
    char *functions = gw_json_raw(root, "functions");
    if (!functions)
        functions = gw_json_raw(root, "tools");
    double temperature = gw_json_num(root, "temperature", api->config.temperature_default);
    int max_tokens = gw_json_int(root, "max_tokens", (int)api->config.max_tokens_default);
    cJSON_Delete(root);

    const char *model = resolve_model(api, req_model);
    if (!model) {
        AIRY_LOG_WARN("chat request carries no model and no default model is configured");
        AIRY_FREE(req_model);
        AIRY_FREE(messages);
        AIRY_FREE(functions);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Missing model\","
                                     "\"type\":\"invalid_request_error\",\"code\":400}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *llm_response = NULL;
    int rc = api->llm_call_fn(model, messages ? messages : "[]", functions ? functions : NULL,
                              temperature, max_tokens, &llm_response, api->llm_call_data);

    if (rc != 0 || !llm_response) {
        AIRY_LOG_ERROR("LLM call failed: model=%s, rc=%d", model, rc);
        AIRY_FREE(llm_response);
        AIRY_FREE(req_model);
        AIRY_FREE(messages);
        AIRY_FREE(functions);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"LLM call failed\","
                                     "\"type\":\"server_error\",\"code\":500}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_IO;
    }

    AIRY_FREE(req_model);
    AIRY_FREE(messages);
    AIRY_FREE(functions);
    *response_json = llm_response;
    sync_atomic_add(&api->tokens_used, (uint64_t)(strlen(llm_response) / 4));
    return 0;
}

static int handle_embeddings(gw_llm_api_t *api, const char *body_json, char **response_json)
{
    if (!api->embed_fn) {
        AIRY_LOG_ERROR("no embedding backend configured for embeddings endpoint");
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"No embedding backend configured\","
                                     "\"type\":\"server_error\",\"code\":503}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_NULL_POINTER;
    }

    cJSON *root = gw_json_load(body_json);
    char *req_model = gw_json_str(root, "model");
    char *input_json = gw_json_raw(root, "input");
    cJSON_Delete(root);

    const char *model = resolve_model(api, req_model);
    if (!model) {
        AIRY_LOG_WARN("embedding request carries no model and no default model is configured");
        AIRY_FREE(req_model);
        AIRY_FREE(input_json);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Missing model\","
                                     "\"type\":\"invalid_request_error\",\"code\":400}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *embed_response = NULL;
    int rc = api->embed_fn(model, input_json ? input_json : "[]", &embed_response, api->embed_data);

    if (rc != 0 || !embed_response) {
        AIRY_LOG_ERROR("embedding call failed: model=%s, rc=%d", model, rc);
        AIRY_FREE(embed_response);
        AIRY_FREE(req_model);
        AIRY_FREE(input_json);
        *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Embedding failed\","
                                     "\"type\":\"server_error\",\"code\":500}}");
        sync_atomic_add(&api->error_count, 1);
        return AIRY_ERR_IO;
    }

    AIRY_FREE(req_model);
    AIRY_FREE(input_json);

    *response_json = embed_response;
    return 0;
}

static int handle_models_list(gw_llm_api_t *api, char **response_json)
{
    if (!api->config.default_model[0]) {
        *response_json = AIRY_STRDUP("{\"object\":\"list\",\"data\":[]}");
        return *response_json ? 0 : AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *resp = "{\"object\":\"list\",\"data\":["
                       "{\"id\":\"%s\",\"object\":\"model\",\"owned_by\":\"agentrt\"}]}";
    size_t len = snprintf(NULL, 0, resp, api->config.default_model);
    char *buf = (char *)AIRY_MALLOC(len + 1);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(buf, len + 1, resp, api->config.default_model);
    *response_json = buf;
    return 0;
}

static bool path_endswith(const char *path, const char *suffix)
{
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    return plen >= slen && memcmp(path + plen - slen, suffix, slen) == 0;
}

static int dispatch_request(gw_llm_api_t *api, const char *path, const char *body_json,
                            char **response_json)
{
    if (path && path_endswith(path, "/v1/models")) {
        return handle_models_list(api, response_json);
    }

    if (path && path_endswith(path, "/chat/completions")) {
        if (!body_json) {
            *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Empty body\","
                                         "\"type\":\"invalid_request_error\",\"code\":400}}");
            sync_atomic_add(&api->error_count, 1);
            return AIRY_ERR_INVALID_PARAM;
        }
        return handle_chat_completions(api, body_json, response_json);
    }

    if (path && path_endswith(path, "/embeddings")) {
        if (!body_json) {
            *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Empty body\","
                                         "\"type\":\"invalid_request_error\",\"code\":400}}");
            sync_atomic_add(&api->error_count, 1);
            return AIRY_ERR_INVALID_PARAM;
        }
        return handle_embeddings(api, body_json, response_json);
    }

    if (body_json && strstr(body_json, "\"messages\"") && strstr(body_json, "\"model\"")) {
        return handle_chat_completions(api, body_json, response_json);
    }

    *response_json = AIRY_STRDUP("{\"error\":{\"message\":\"Unknown LLM API endpoint\","
                                 "\"type\":\"invalid_request_error\",\"code\":404}}");
    sync_atomic_add(&api->error_count, 1);
    return AIRY_ERR_NOT_FOUND;
}

static int llm_api_request(const char *method, const char *path, const char *body_json,
                                  char **response_json, void *user_data)
{
    (void)method;
    gw_llm_api_t *api = (gw_llm_api_t *)user_data;
    if (!api || !response_json)
        return AIRY_ERR_INVALID_PARAM;
    sync_atomic_add(&api->request_count, 1);
    return dispatch_request(api, path, body_json, response_json);
}

gw_proto_request_handler_t gw_llm_get_handler(gw_llm_api_t *api)
{
    if (!api) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    return llm_api_request;
}

void *gw_llm_get_hdata(gw_llm_api_t *api)
{
    return (void *)api;
}
