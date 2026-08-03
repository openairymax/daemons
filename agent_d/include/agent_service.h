// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
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
 * @return AIRY_SUCCESS 成功，*out_output 输出 JSON 结果字符串（调用方负责 AIRY_FREE）；
 *         AIRY_ERR_NOT_FOUND Agent 不存在；
 *         AIRY_ERR_STATE_ERROR Agent 未运行（已终止）
 */
int agent_service_invoke(agent_service_t *svc, const char *agent_id,
                          const char *input, size_t len, char **out_output);

/**
 * @brief 列出所有 Agent ID
 * @return AIRY_SUCCESS 成功，*out_agent_ids 输出 ID 数组，*out_count 输出数量
 *         （调用方负责 agent_service_list_free 释放）
 */
int agent_service_list(agent_service_t *svc, char ***out_agent_ids,
                         size_t *out_count);

/* ---------- 辅助接口 ---------- */

size_t agent_service_count(agent_service_t *svc);
void agent_service_list_free(char **agent_ids, size_t count);

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
