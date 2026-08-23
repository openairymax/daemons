/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_service.h
 * @brief Public LLM service interface.
 */

#ifndef AIRY_RT_LLM_SERVICE_H
#define AIRY_RT_LLM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct llm_service llm_service_t;

typedef struct {
    const char *role;
    const char *content;
    /* Reasoning trace (reasoning models, e.g. DeepSeek-R1 / Kimi-K2):
     * - Request side: an assistant message produced by a reasoning model must
     *   echo its reasoning_content when the turn is re-sent; DeepSeek and
     *   Kimi reject a tool-loop request with HTTP 400 if the field is
     *   dropped between turns.
     * - Response side: choices[].reasoning_content carries the reasoning
     *   trace of the current completion.
     * NULL when the message/choice has no reasoning. */
    const char *reasoning_content;
    /* Function calling (OpenAI-compatible):
     * - role="tool" messages carry tool_call_id (matching the assistant's
     *   tool_call id)
     * - role="assistant" messages may carry tool_calls (JSON array string
     *   with id/type/function.name/function.arguments) */
    const char *tool_call_id;
    const char *tool_calls_json;
} llm_message_t;

typedef struct {
    const char *model;
    const llm_message_t *messages;
    size_t message_count;
    float temperature;
    float top_p;
    int max_tokens;
    int stream;
    const char **stop;
    size_t stop_count;
    double presence_penalty;
    double frequency_penalty;
    /* JSON string of the OpenAI tools array (function-calling tool
     * definitions, e.g. [{"type":"function","function":{"name":"fs_read",
     * "parameters":{...}}}]) */
    const char *tools_json;
    void *user_data;
} llm_request_config_t;

typedef struct {
    char *id;
    char *model;
    llm_message_t *choices;
    size_t choice_count;
    uint64_t created;
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    /* Thinking (reasoning) tokens reported by the upstream usage block
     * (e.g. DeepSeek/OpenAI completion_tokens_details.reasoning_tokens).
     * Zero when the upstream does not report it. */
    uint32_t reasoning_tokens;
    double cost_usd;
    char *finish_reason;
} llm_response_t;

typedef void (*llm_stream_callback_t)(const char *chunk, void *user_data);


llm_service_t *llm_service_create(const char *config_path);
void llm_service_destroy(llm_service_t *svc);


int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response);

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response);

void llm_response_free(llm_response_t *resp);


int llm_service_stats(llm_service_t *svc, char **out_json);


/**
 * @brief Return the list of all models available in the registry (JSON
 *        string, caller AIRY_FREEs).
 *
 * @param svc Service context
 * @return JSON like {"models":[{"name","provider","default"}],
 *         "default_model","default_provider"}; NULL if svc is NULL or OOM
 */
char *llm_service_list_models(llm_service_t *svc);


/**
 * @brief Return the service default model name (global.default_model,
 *        main config + user override).
 * @param svc Service context
 * @return Default model name; NULL if unset or svc is NULL (not to be freed)
 */
const char *llm_service_default_model(const llm_service_t *svc);

/**
 * @brief Proxy an OpenAI-compatible embeddings request to the provider.
 *
 * The request body ({"model":..,"input":..}) is forwarded verbatim to
 * $api_base/embeddings of the provider that owns @a model (default model
 * when NULL/empty), returning the upstream OpenAI-format JSON untouched.
 *
 * @param svc   Service context
 * @param model Model name; NULL/empty → default model
 * @param request_body OpenAI embeddings request JSON (non-empty)
 * @param out_json     Malloc'd upstream response JSON (caller AIRY_FREEs)
 * @return AIRY_SUCCESS on success; *out_json holds the upstream response
 */
int llm_service_embeddings(llm_service_t *svc, const char *model, const char *request_body,
                           char **out_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SERVICE_H */