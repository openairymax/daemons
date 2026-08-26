/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_model_defaults.h
 * @brief Default-model extraction from the global / think sections of
 *        model.yaml (shared by llm_d / gateway_d / think_d).
 *
 * The user model-config SSoT (ecosystem/manager/model/model.yaml) carries
 * default_provider / default_model in its global section, and the dual-think
 * (Thinkdual) three-role model mapping (s2 / verify / expert) in its think
 * section. llm_d consumes this file to build the provider registry;
 * gateway_d needs the same default-model semantics (AI-agent orchestration
 * default model); think_d needs the think section for the t2/t1-f/t1-p
 * model injection. This API centralizes "extract config sections from YAML"
 * in the daemon common layer, so each daemon avoids its own libyaml scan
 * logic (single source).
 *
 * A user override file $AIRY_CONFIG_DIR/model.yaml is also supported (when
 * merged with the repo SSoT, user wins for same-named global fields); both
 * are read through this API and the caller decides precedence.
 */

#ifndef SVC_MODEL_DEFAULTS_H
#define SVC_MODEL_DEFAULTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Extract default_model / default_provider from the global section
 *        of model.yaml.
 *
 * libyaml event-stream scan: only the default_model and default_provider
 * keys inside the top-level global: mapping are considered. Missing fields
 * leave the output buffer empty (not an error); unreadable/missing file
 * returns AIRY_ERR_IO; builds without libyaml support return
 * AIRY_ERR_NOT_SUPPORTED.
 *
 * @param path          YAML file path
 * @param out_model     Output buffer for default_model (may be NULL to skip)
 * @param model_sz      out_model buffer size
 * @param out_provider  Output buffer for default_provider (may be NULL to skip)
 * @param prov_sz       out_provider buffer size
 * @return 0 on success; AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_from_yaml(const char *path, char *out_model, size_t model_sz,
                                 char *out_provider, size_t prov_sz);

/**
 * @brief Dual-think (Thinkdual) three-model configuration (think section).
 *
 * When enabled=1, think_d uses the GRAD plan-level critique loop; an empty
 * model name means "use the provider default" (global.default_model).
 * timeout_ms=0 means use think_d's built-in default (120000).
 */
typedef struct {
    int enabled;
    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
    uint32_t timeout_ms;
} svc_model_think_config_t;

/**
 * @brief Extract the dual-think three-model config from the think section.
 *
 * Same-origin state machine as svc_model_defaults_from_yaml: only the
 * enabled / think2_slow_model / think1_fast_model / think1_prof_model /
 * timeout_ms keys inside the top-level think: mapping are considered.
 * If no think section is found, out keeps its initial value
 * (enabled=1, empty model names, timeout=0); not an error (legacy config
 * compatibility).
 *
 * @param path YAML file path
 * @param out  Output struct (non-NULL; caller should zero it first)
 * @return 0 on success; AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_think_from_yaml(const char *path, svc_model_think_config_t *out);

/**
 * @brief Simplified LLM config (llm section of model.yaml).
 *
 * Minimal config entry for ordinary users: one provider + one default
 * model. api_format is one of two: openai (OpenAI Chat Completions) /
 * anthropic (Anthropic Messages). base_url is the adapter root (DeepSeek
 * official dual protocol: openai-form e.g. https://..., anthropic-form
 * https://...). When an llm section exists and model is non-empty, llm_d
 * expands it at runtime into a single provider (taking precedence over the
 * full providers/models list).
 */
typedef struct {
    char api_format[16]; /* llm.api_format: openai | anthropic */
    char base_url[512];
    char api_key_env[128];
    char model[128];
} svc_model_llm_config_t;

/**
 * @brief Extract the simplified LLM config from the llm section.
 *
 * Same-origin libyaml state machine: only the api_format / base_url /
 * api_key_env / model keys inside the top-level llm: mapping are
 * considered. If no llm section is found, out keeps the caller's initial
 * value; not an error.
 *
 * @param path YAML file path
 * @param out  Output struct (non-NULL; caller should zero it first)
 * @return 0 on success; AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_llm_from_yaml(const char *path, svc_model_llm_config_t *out);

/**
 * @brief Extract the primary connection config from the models table
 *        (v2 table format, 2026-08-26).
 *
 * Reads the first item of the top-level models: list (api_format /
 * base_url / api_key_env / model_id). Fallback source for llm_d /
 * gateway_d when the llm section is absent; models[0].model_id is the
 * default model.
 *
 * @param path YAML file path
 * @param out  Output struct (non-NULL; caller should zero it first)
 * @return 0 on success; AIRY_ERR_INVALID_PARAM / AIRY_ERR_IO /
 *         AIRY_ERR_NOT_FOUND / AIRY_ERR_NOT_SUPPORTED
 */
int svc_model_defaults_models0_from_yaml(const char *path, svc_model_llm_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SVC_MODEL_DEFAULTS_H */
