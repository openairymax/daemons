/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_service.h
 * @brief Public LLM service interface.
 */

#ifndef AIRY_RT_LLM_SERVICE_H
#define AIRY_RT_LLM_SERVICE_H

#include "llm_service_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


llm_service_t *llm_service_create(const char *config_path);
void llm_service_destroy(llm_service_t *svc);


int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response);

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response);


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
 * @brief Return the role slots (tc3) view as JSON (caller AIRY_FREEs).
 *
 * 字段：declared 为配置面原值（""=未显式配置），effective 为
 * 回落 default_model 后的实际取值，degraded 表示存在未显式配置的角色。
 *
 * @param svc Service context
 * @return JSON like {"slots":{"t2":{"declared","effective"},...},
 *         "degraded":bool}; NULL if svc is NULL or OOM
 */
char *llm_slots_get(llm_service_t *svc);

/**
 * @brief Bind one role slot to a registered model and persist it.
 *
 * 强校验：角色名、model_id 语法与「必须已在注册表」三关任一不过即拒绝，
 * 并把可选 model_id 写进 @a err。通过后写入 model.yaml 的 slots: 段并触
 * 发热重载，使 llm_d 与 think_d 在同一次改动后看到同一份槽位。
 *
 * @param svc   Service context
 * @param role  "t2" | "t1f" | "t1p"
 * @param model Registered model_id；空串表示清空该角色（回落 default_model）
 * @param err   失败原因（含可选值），调用方保证非空
 * @param cap   err 容量
 * @return 0 已落盘并生效；AIRY_ERR_INVALID_PARAM 校验失败；
 *         AIRY_ERR_IO 写盘失败；AIRY_ERR_STATE_ERROR 写盘成功但重载被拒
 */
int llm_slots_set(llm_service_t *svc, const char *role, const char *model, char *err,
                          size_t cap);


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
