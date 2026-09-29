/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_d_internal.h
 * @brief llm_d 装配域唯一私有头（gen5 五件套）。
 *
 * 承载 src/svc.c（生命周期与端点策略）、src/rpc/methods.c（RPC 方法面）与
 * src/rpc/dispatch.c（请求解析）之间的共享符号。端点常量由生成头
 * svc_llm_d.h 提供（wire 契约 SSoT），本头不重复声明。跨域禁止 include
 * 本头（引用律）。
 */

#ifndef AIRY_RT_DAEMON_LLM_D_INTERNAL_H
#define AIRY_RT_DAEMON_LLM_D_INTERNAL_H

#include "llm_service.h"

#include <cjson/cJSON.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单请求 messages 上限（容量与拒绝判据共用同一常量，防口径漂移）。 */
#define MAX_MESSAGES_PER_REQUEST 128

/* 服务单例：由 src/svc.c 定义并持有生命周期。 */
extern llm_service_t *g_service;

/* 单请求上下文：messages 与响应缓冲由 request_context_create 分配、
 * request_context_destroy 释放（成对，资源确定性）。 */
typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
    char *tools_json;
    /* parse_params 失败的具体原因（如 "messages 缺失/为空数组"、"model
     * 未配置且无默认模型"）。随 -32602 错误消息透传，避免笼统的
     * "Invalid params" 掩盖失败环节。 */
    char fail_reason[160];
} request_context_t;

/* 请求解析域（src/rpc/dispatch.c） */
request_context_t *request_context_create(void);
void request_context_destroy(request_context_t *ctx);
int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_LLM_D_INTERNAL_H */
