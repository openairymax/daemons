/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file internal.h
 * @brief llm_d 进程装配域（bootstrap）内部声明。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入。
 * 仅 daemon 四件（main.c / llm_daemon_request.c / llm_daemon_methods.c /
 * llm_daemon_config.c）消费；跨域禁止 include 本头。
 */

#ifndef AIRY_RT_LLM_BOOTSTRAP_INTERNAL_H
#define AIRY_RT_LLM_BOOTSTRAP_INTERNAL_H

#include "contract.h"
#include "service.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Daemon application domain (main.c split, 2026-08-27) ----
 *
 * Shared constants/types/globals between main.c (entry & wiring),
 * llm_daemon_request.c (params parsing), llm_daemon_methods.c (RPC methods)
 * and llm_daemon_config.c (daemon config assembly). The daemon_main.h
 * generated boilerplate (g_running_llm_d etc.) stays static inside main.c —
 * only symbols that cross those four files are declared here. */

extern llm_service_t *g_service;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_threads;
    int max_clients;
} llm_daemon_config_t;

extern llm_daemon_config_t g_config;

typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
    char *tools_json;
    /* parse_params 失败的具体原因（"messages 缺失/为空数组"、"model 未配置
     * 且无默认模型" 等）。此前 complete/complete_stream 一律回 -32602
     * "Invalid params"，客户端与用户都无法区分失败环节，社区反馈（ubuntu
     * airymaxrt 问答直接打印裸 JSON-RPC 错误）只能靠猜测定位。填充后随
     * -32602 错误消息透传，使故障可自助识别。 */
    char fail_reason[160];
} request_context_t;

/* llm_daemon_request.c */
request_context_t *request_context_create(void);
void request_context_destroy(request_context_t *ctx);
int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg);

/* llm_daemon_config.c */
int load_daemon_config(const char *config_path);
void free_daemon_config(void);
void destroy_service_llm_d(void);

/* llm_daemon_methods.c: JSON-RPC method adapters registered on the
 * dispatcher by main() */
void on_complete_method(cJSON *params, int id, void *user_data);
void on_complete_stream_method(cJSON *params, int id, void *user_data);
void on_list_models_method(cJSON *params, int id, void *user_data);
void on_embeddings_method(cJSON *params, int id, void *user_data);
void on_count_tokens_method(cJSON *params, int id, void *user_data);
void on_health_check_method(cJSON *params, int id, void *user_data);
void on_get_stats_method(cJSON *params, int id, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_BOOTSTRAP_INTERNAL_H */
