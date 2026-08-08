// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file llm_service.h
 * @brief LLM 服务对外接口
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_LLM_SERVICE_H
#define AIRY_RT_LLM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 公共类型定义 ---------- */

typedef struct llm_service llm_service_t;

typedef struct {
    const char *role;
    const char *content;
    /* Function calling（OpenAI 兼容）：
     * - role="tool" 的消息携带 tool_call_id（对应 assistant 的 tool_call id）
     * - role="assistant" 的消息可携带 tool_calls（JSON 数组字符串，含
     *   id/type/function.name/function.arguments） */
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
    /* OpenAI tools 数组的 JSON 字符串（function calling 工具定义，
     * 如 [{"type":"function","function":{"name":"fs_read","parameters":{...}}}]） */
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
    double cost_usd; /* 单次调用估算成本（USD），由 llm_d 按模型单价计算，网关/TUI 据此累计 */
    char *finish_reason;
} llm_response_t;

typedef void (*llm_stream_callback_t)(const char *chunk, void *user_data);

/* ---------- 生命周期 ---------- */

llm_service_t *llm_service_create(const char *config_path);
void llm_service_destroy(llm_service_t *svc);

/* ---------- 请求接口 ---------- */

int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response);

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response);

void llm_response_free(llm_response_t *resp);

/* ---------- 统计 ---------- */

int llm_service_stats(llm_service_t *svc, char **out_json);

/* ---------- 模型列表（A2-3: llm.list_models） ---------- */

/**
 * @brief 返回 registry 中全部可用模型列表（JSON 字符串，调用者 AIRY_FREE）
 *
 * @param svc 服务上下文
 * @return 形如 {"models":[{"name","provider","default"}],"default_model","default_provider"}
 *         的 JSON；svc 为 NULL 或内存不足返回 NULL
 */
char *llm_service_list_models(llm_service_t *svc);

/* ---------- 默认模型（A2-2: 缺省回落） ---------- */

/**
 * @brief 返回服务默认模型名（global.default_model，主配置 + 用户覆盖）
 * @param svc 服务上下文
 * @return 默认模型名；未配置或 svc 为 NULL 返回 NULL（不可 free）
 */
const char *llm_service_default_model(const llm_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SERVICE_H */