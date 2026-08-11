/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file a2a_service.h
 * @brief A2A 服务对外接口（a2a.* 命名空间）
 *
 * 封装 a2a_v03_adapter 库，作为 a2a_d 守护进程的服务核心对外暴露。
 * 提供智能体注册/发现、任务生命周期管理、智能体间消息传递能力。
 *
 */

#ifndef AIRY_RT_A2A_SERVICE_H
#define AIRY_RT_A2A_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a2a_service a2a_service_t;


/**
 * @brief 创建 A2A 服务实例
 * @param max_agents 最大智能体数（0 表示使用默认值 256）
 * @param max_tasks 最大任务数（0 表示使用默认值 4096）
 * @return 服务实例指针，失败返回 NULL
 */
a2a_service_t *a2a_service_create(size_t max_agents, size_t max_tasks);
void a2a_service_destroy(a2a_service_t *svc);


/**
 * @brief 注册智能体
 * @param card_json Agent Card JSON 字符串，字段：
 *        id, name, description, url, version, protocol_version(int,默认3),
 *        capabilities(int), available(bool,默认true), skills(数组,可选)
 * @return AIRY_SUCCESS 成功
 */
int a2a_service_register_agent(a2a_service_t *svc, const char *card_json);

/**
 * @brief 注销智能体
 * @return AIRY_SUCCESS 成功，AIRY_ERR_NOT_FOUND 未找到
 */
int a2a_service_unregister_agent(a2a_service_t *svc, const char *agent_id);

/**
 * @brief 获取智能体卡片
 * @return AIRY_SUCCESS 成功，*out_card_json 输出卡片 JSON 字符串
 *         （调用方负责 a2a_service_card_free 释放）；
 *         AIRY_ERR_NOT_FOUND 未找到
 */
int a2a_service_get_agent_card(a2a_service_t *svc, const char *agent_id, char **out_card_json);

/**
 * @brief 发现智能体
 * @param capability 能力过滤字符串，可为 NULL（不过滤）
 * @param skill_name 技能过滤字符串，可为 NULL（不过滤）
 * @return AIRY_SUCCESS 成功，*out_results_json 输出卡片 JSON 数组字符串
 *         （调用方负责 a2a_service_results_free 释放），*out_count 输出命中数
 */
int a2a_service_discover_agents(a2a_service_t *svc, const char *capability, const char *skill_name,
                                char **out_results_json, size_t *out_count);


/**
 * @brief 创建任务
 * @return AIRY_SUCCESS 成功，*out_task_json 输出任务 JSON 字符串
 *         （调用方负责 a2a_service_task_free 释放）
 */
int a2a_service_create_task(a2a_service_t *svc, const char *agent_id, const char *description,
                            const char *input_json, char **out_task_json);

/**
 * @brief 更新任务状态
 * @param state 新状态值（a2a_task_state_t 枚举）
 * @param output_json 输出 JSON，可为 NULL
 * @param progress 进度 [0.0, 1.0]
 * @return AIRY_SUCCESS 成功，AIRY_ERR_NOT_FOUND 未找到
 */
int a2a_service_update_task(a2a_service_t *svc, const char *task_id, int state,
                            const char *output_json, double progress);

/**
 * @brief 取消任务
 * @param reason 取消原因，可为 NULL
 * @return AIRY_SUCCESS 成功，AIRY_ERR_NOT_FOUND 未找到
 */
int a2a_service_cancel_task(a2a_service_t *svc, const char *task_id, const char *reason);

/**
 * @brief 获取任务
 * @return AIRY_SUCCESS 成功，*out_task_json 输出任务 JSON 字符串
 *         （调用方负责 a2a_service_task_free 释放）；
 *         AIRY_ERR_NOT_FOUND 未找到
 */
int a2a_service_get_task(a2a_service_t *svc, const char *task_id, char **out_task_json);


/**
 * @brief 发送消息到目标智能体
 * @param role 消息角色（如 "user"）
 * @param content_json 消息内容 JSON 字符串
 * @return AIRY_SUCCESS 成功，*out_response_json 输出响应 JSON 数组字符串
 *         （调用方负责 a2a_service_results_free 释放），
 *         *out_response_count 输出响应数
 */
int a2a_service_send_message(a2a_service_t *svc, const char *target_agent_id, const char *role,
                             const char *content_json, char **out_response_json,
                             size_t *out_response_count);


size_t a2a_service_count(a2a_service_t *svc);
size_t a2a_service_task_count(a2a_service_t *svc);

void a2a_service_card_free(char *card_json);
void a2a_service_task_free(char *task_json);
void a2a_service_results_free(char *results_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_A2A_SERVICE_H */
