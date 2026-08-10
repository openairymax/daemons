// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file agent_service.h
 * @brief Agent 服务对外接口（agent.* 命名空间）
 *
 * 承载原 syscall_router.c 中 airy_sys_agent_spawn/terminate/invoke/list
 * 的运行时 Agent 管理逻辑，作为 agent_d 守护进程的服务核心对外暴露。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_AGENT_SERVICE_H
#define AIRY_RT_AGENT_SERVICE_H

#include <stddef.h>
#include <stdint.h>

/* 改进1（异步可中断）：invoke 支持取消令牌（select/poll 非阻塞 + token 轮询） */
#include "cancel_token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_service agent_service_t;

/* ---------- 生命周期 ---------- */

agent_service_t *agent_service_create(size_t max_agents);
void agent_service_destroy(agent_service_t *svc);

/* ---------- Agent 管理接口 ---------- */

/**
 * @brief 派生新 Agent
 * @param spec Agent 规格（JSON 字符串）
 * @return AIRY_SUCCESS 成功，*out_agent_id 输出新 Agent ID（调用方负责 AIRY_FREE）
 */
int agent_service_spawn(agent_service_t *svc, const char *spec,
                          char **out_agent_id);

/**
 * @brief 终止指定 Agent
 * @return AIRY_SUCCESS 成功，AIRY_ERR_NOT_FOUND 未找到
 */
int agent_service_terminate(agent_service_t *svc, const char *agent_id);

/**
 * @brief 调用指定 Agent
 * @param cancel_token [in] 取消令牌（可为 NULL）：invoke 阻塞读响应期间
 *        短轮询该令牌，命中则优雅终止子进程（SIGTERM→2s→SIGKILL）并以
 *        AbortedOutput 收尾，返回 AIRY_ERR_CANCELED（区别于超时路径）
 * @return AIRY_SUCCESS 成功，*out_output 输出 JSON 结果字符串（调用方负责 AIRY_FREE）；
 *         AIRY_ERR_NOT_FOUND Agent 不存在；
 *         AIRY_ERR_STATE_ERROR Agent 未运行（已终止）；
 *         AIRY_ERR_CANCELED 执行被取消（AbortedOutput）
 */
int agent_service_invoke(agent_service_t *svc, const char *agent_id,
                          const char *input, size_t len,
                          airy_cancel_token_t *cancel_token,
                          char **out_output);

/**
 * @brief 列出所有 Agent ID
 * @return AIRY_SUCCESS 成功，*out_agent_ids 输出 ID 数组，*out_count 输出数量
 *         （调用方负责 agent_service_list_free 释放）
 */
int agent_service_list(agent_service_t *svc, char ***out_agent_ids,
                         size_t *out_count);

/* ---------- invoke 会话管理（改进1 "取消下探"：跨进程取消） ---------- */

/**
 * @brief 注册 invoke 会话（request_id → cancel_token）
 *
 * 跨进程取消的会话基础：RPC 层 handle_invoke 在调用 agent_service_invoke
 * 前注册会话，调用方可通过 agent.cancel（RPC）按 request_id 取消。取消
 * 命中后 agent_service_invoke 内 select 轮询感知 token → SIGTERM→SIGKILL
 * 子进程 → 以 AbortedOutput 收尾（与超时 -2 区分）。
 *
 * @param svc 服务实例（非 NULL）
 * @param request_id 唯一请求 ID（非 NULL，≤63 字符）
 * @param out_token [out] 输出本会话的取消令牌指针（BORROW，会话注销前有效）
 * @return AIRY_SUCCESS 注册成功；AIRY_ERR_INVALID_PARAM 参数非法；
 *         AIRY_ERR_BUSY 会话表已满
 */
int agent_service_invoke_begin(agent_service_t *svc, const char *request_id,
                                airy_cancel_token_t **out_token);

/**
 * @brief 注销 invoke 会话（invoke 完成/失败/取消后调用）
 * @param svc 服务实例
 * @param request_id 注册时使用的请求 ID
 */
void agent_service_invoke_end(agent_service_t *svc, const char *request_id);

/**
 * @brief 按 request_id 取消活跃 invoke 会话
 * @param svc 服务实例
 * @param request_id 请求 ID
 * @return AIRY_SUCCESS 找到会话并已请求取消；
 *         AIRY_ERR_NOT_FOUND 无匹配活跃会话
 */
int agent_service_invoke_cancel(agent_service_t *svc, const char *request_id);

/* ---------- 辅助接口 ---------- */

size_t agent_service_count(agent_service_t *svc);
void agent_service_list_free(char **agent_ids, size_t count);

/* ---------- 性能监控 ---------- */

/**
 * @brief 服务性能统计快照（10000 并发场景下由 agent_d 监控线程周期采样）
 *
 * 所有字段均为自服务创建以来的累计值；spawn/invoke 耗时为微秒聚合。
 * 各字段以原子计数更新，读取无需持锁（宽松一致即可满足监控语义）。
 */
typedef struct {
    int spawn_total;         /* spawn 请求总数 */
    int spawn_ok;            /* spawn 成功数 */
    int spawn_fail;          /* spawn 失败数 */
    int invoke_total;        /* invoke 请求总数 */
    int invoke_ok;           /* invoke 成功数 */
    int invoke_fail;         /* invoke 失败数 */
    int terminate_total;     /* terminate 请求总数 */
    int lock_wait_total;     /* 全局锁 trylock 探测失败次数（锁竞争信号） */
    int peak_running;        /* 峰值并发 running agent 数 */
    unsigned long long spawn_us_total; /* spawn 累计耗时（微秒） */
    unsigned long long spawn_us_max;   /* spawn 单次最大耗时（微秒） */
    unsigned long long invoke_us_total;/* invoke 累计耗时（微秒） */
    unsigned long long invoke_us_max;  /* invoke 单次最大耗时（微秒） */
} agent_perf_stats_t;

/**
 * @brief 获取服务性能统计快照
 * @param svc  Agent 服务实例
 * @param out  输出统计（非 NULL）
 * @return AIRY_SUCCESS 成功
 */
int agent_service_get_perf(agent_service_t *svc, agent_perf_stats_t *out);

/**
 * @brief 回收空闲超时的 Agent 子进程（P0-3：防止子进程泄漏）
 *
 * 遍历所有 running 且有活跃子进程的 Agent，当 now - last_active >=
 * max_idle_s 时终止并回收其子进程，Agent 槽位置为 terminated（不压缩数组）。
 *
 * @param svc Agent 服务实例
 * @param max_idle_s 空闲阈值（秒），0 表示立即回收
 * @return AIRY_SUCCESS 成功
 */
int agent_service_reap_idle(agent_service_t *svc, uint64_t max_idle_s);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_AGENT_SERVICE_H */
