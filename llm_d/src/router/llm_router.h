/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_router.h
 * @brief LLM 路由器接口
 *
 * LLM Router 负责根据请求特征（复杂度、成本、延迟等）
 * 将 LLM 请求路由到最合适的提供商和模型。
 *
 * 路由策略：
 *   - COMPLEXITY_BASED:  基于任务复杂度路由
 *   - COST_OPTIMIZED:    成本优化路由
 *   - LATENCY_OPTIMIZED: 延迟优化路由
 *   - FALLBACK:          降级路由（主提供商失败时切换）
 *   - ROUND_ROBIN:       轮询路由
 *
 * @owner team-A
 * @see contracts/contract_A_B.h 第3节（协议路由表）
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    LLM_ROUTE_COMPLEXITY = 0,
    LLM_ROUTE_COST = 1,
    LLM_ROUTE_LATENCY = 2,
    LLM_ROUTE_FALLBACK = 3,
    LLM_ROUTE_ROUND_ROBIN = 4,
    LLM_ROUTE_COUNT = 5
} llm_route_strategy_t;


typedef enum {
    LLM_CAP_CHAT = 0x0001,
    LLM_CAP_COMPLETION = 0x0002,
    LLM_CAP_EMBEDDING = 0x0004,
    LLM_CAP_FUNCTION_CALL = 0x0008,
    LLM_CAP_VISION = 0x0010,
    LLM_CAP_STREAMING = 0x0020,
    LLM_CAP_JSON_MODE = 0x0040,
    LLM_CAP_EXTENDED_THINK = 0x0080,
    LLM_CAP_CODE_EXEC = 0x0100
} llm_capability_t;


typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    char api_key_env[64];
    uint32_t capabilities;
    uint32_t context_window;
    double cost_per_1k_input;
    double cost_per_1k_output;
    uint32_t avg_latency_ms;
    uint32_t rate_limit_rpm;
    bool enabled;
    int priority;
} llm_endpoint_t;


typedef struct {
    const char *prompt;
    size_t prompt_len;
    uint32_t required_caps;
    uint32_t max_tokens;
    double max_cost;
    uint32_t max_latency_ms;
    llm_route_strategy_t strategy;
    char preferred_provider[64];
} llm_route_request_t;


typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    double estimated_cost;
    uint32_t estimated_latency_ms;
    llm_route_strategy_t strategy_used;
    int confidence;
    char fallback_provider[64];
    char fallback_model[64];
} llm_route_result_t;


typedef struct {
    uint64_t total_requests;
    uint64_t routed_count[5];
    uint64_t fallback_count;
    uint64_t error_count;
    double total_cost;
    uint64_t total_tokens;
} llm_router_stats_t;


/**
 * @brief 初始化 LLM 路由器
 * @param config_path 配置文件路径
 * @return 0 成功，非0失败
 */
int llm_router_init(const char *config_path);

/**
 * @brief 销毁 LLM 路由器
 */
void llm_router_destroy(void);

/**
 * @brief 注册提供商端点
 * @param endpoint 端点信息
 * @return 0 成功，非0失败
 */
int llm_router_register_endpoint(const llm_endpoint_t *endpoint);

/**
 * @brief 注销提供商端点
 * @param provider_name 提供商名称
 * @param model_name    模型名称
 * @return 0 成功，非0失败
 */
int llm_router_unregister_endpoint(const char *provider_name, const char *model_name);

/**
 * @brief 路由 LLM 请求
 * @param request 路由请求
 * @param result  路由结果
 * @return 0 成功，非0失败
 */
int llm_router_route(const llm_route_request_t *request, llm_route_result_t *result);

/**
 * @brief 获取路由器统计
 * @param stats 输出统计
 * @return 0 成功，非0失败
 */
int llm_router_get_stats(llm_router_stats_t *stats);

/**
 * @brief 设置默认路由策略
 * @param strategy 路由策略
 * @return 0 成功，非0失败
 */
int llm_router_set_default_strategy(llm_route_strategy_t strategy);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H */
