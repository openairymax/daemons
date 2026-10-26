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
#include "providers/core/ep_health.h"
#include "providers/core/registry.h"

#include <cjson/cJSON.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单请求 messages 上限（容量与拒绝判据共用同一常量，防口径漂移）。 */
#define MAX_MESSAGES_PER_REQUEST 128

/* 服务单例：由 src/svc.c 定义并持有生命周期。 */
extern llm_service_t *g_service;

/* 端点健康视图单例：由 src/svc.c 在 svc_activate 建立、svc_destroy 释放。
 * 定义与生命周期归装配域，消费侧（methods.c 的 health_check 与流式冷加载
 * 上报）只读经此句柄。 */
extern ep_health_t *g_ep;

/* 跨域桥：llm_service 的 provider registry 句柄。struct llm_service 是 rpc
 * 域私有类型（service.h），装配域与 RPC 方法域不透明持有 g_service，无法直
 * 接取 registry；本桥在 rpc 域（service.c）实现，仅暴露 registry 句柄，
 * 保持 struct llm_service 私有不被穿透。 */
provider_registry_t *llm_svc_reg(const llm_service_t *svc);

/* 单请求上下文：messages 与 tools_json 由 request_context_create 分配、
 * request_context_destroy 释放（成对，资源确定性）。 */
typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *tools_json;
    /* C11 grammar 串的本体所有权（cfg->grammar 只借用指针）。 */
    char *grammar;
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
