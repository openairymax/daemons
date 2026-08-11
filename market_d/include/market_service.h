/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file market_service.h
 * @brief 市场服务接口定义
 * @details 负责 Agent 和 Skill 的注册、发现、安装和管理
 */

#ifndef AIRY_RT_MARKET_SERVICE_H
#define AIRY_RT_MARKET_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 市场服务配置
 */
typedef struct {
    char *registry_url;
    char *storage_path;
    uint32_t sync_interval_ms;
    uint32_t cache_ttl_ms;
    bool enable_remote_registry;
    bool enable_auto_update;
} market_config_t;

/**
 * @brief Agent 类型
 */
typedef enum {
    AGENT_TYPE_ASSISTANT,
    AGENT_TYPE_EXPERT,
    AGENT_TYPE_SPECIALIZED,
    AGENT_TYPE_CUSTOM,
    AGENT_TYPE_COUNT
} agent_type_t;

/**
 * @brief Agent 状态
 */
typedef enum {
    AGENT_STATUS_AVAILABLE,
    AGENT_STATUS_INSTALLING,
    AGENT_STATUS_ERROR,
    AGENT_STATUS_DISABLED,
    AGENT_STATUS_COUNT
} agent_status_t;

/**
 * @brief Skill 类型
 */
typedef enum {
    SKILL_TYPE_TOOL,
    SKILL_TYPE_KNOWLEDGE,
    SKILL_TYPE_INTEGRATION,
    SKILL_TYPE_CUSTOM,
    SKILL_TYPE_COUNT
} skill_type_t;

/**
 * @brief Agent 信息
 */
typedef struct {
    char *agent_id; /**< Agent ID */
    char *name;
    char *version;
    char *description;
    agent_type_t type;
    agent_status_t status;
    char *author;
    char *repository;
    char *dependencies;
    float rating;
    uint32_t download_count;
    uint64_t last_updated;
} agent_info_t;

/**
 * @brief Skill 信息
 */
typedef struct {
    char *skill_id; /**< Skill ID */
    char *name;
    char *version;
    char *description;
    skill_type_t type;
    char *author;
    char *repository;
    char *dependencies;
    float rating;
    uint32_t download_count;
    uint64_t last_updated;
} skill_info_t;

/**
 * @brief 安装请求
 */
typedef struct {
    char *id;
    char *version;
    bool force_update;
    char *install_path;
} install_request_t;

/**
 * @brief 安装结果
 */
typedef struct {
    bool success;
    char *message;
    char *installed_version;
    char *install_path;
    int error_code;
} install_result_t;

/**
 * @brief 搜索参数
 */
typedef struct {
    char *query;
    agent_type_t agent_type;
    skill_type_t skill_type;
    bool only_installed;
    bool sort_by_rating;
    bool sort_by_download;
    size_t limit;
    size_t offset;
} search_params_t;

/**
 * @brief 市场服务句柄
 */
typedef struct market_service market_service_t;

/**
 * @brief 创建市场服务
 * @param manager 配置信息
 * @param service 输出参数，返回创建的服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_create(const market_config_t *manager, market_service_t **service);

/**
 * @brief 销毁市场服务
 * @param service 服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_destroy(market_service_t *service);

/**
 * @brief 注册 Agent
 * @param service 服务句柄
 * @param agent_info Agent 信息
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_register_agent(market_service_t *service, const agent_info_t *agent_info);

/**
 * @brief 注册 Skill
 * @param service 服务句柄
 * @param skill_info Skill 信息
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_register_skill(market_service_t *service, const skill_info_t *skill_info);

/**
 * @brief 搜索 Agent
 * @param service 服务句柄
 * @param params 搜索参数
 * @param agents 输出参数，返回 Agent 信息数组
 * @param count 输出参数，返回 Agent 数量
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_search_agents(market_service_t *service, const search_params_t *params,
                                 agent_info_t ***agents, size_t *count);

/**
 * @brief 搜索 Skill
 * @param service 服务句柄
 * @param params 搜索参数
 * @param skills 输出参数，返回 Skill 信息数组
 * @param count 输出参数，返回 Skill 数量
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_search_skills(market_service_t *service, const search_params_t *params,
                                 skill_info_t ***skills, size_t *count);

/**
 * @brief 安装 Agent
 * @param service 服务句柄
 * @param request 安装请求
 * @param result 输出参数，返回安装结果
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_install_agent(market_service_t *service, const install_request_t *request,
                                 install_result_t **result);

/**
 * @brief 安装 Skill
 * @param service 服务句柄
 * @param request 安装请求
 * @param result 输出参数，返回安装结果
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_install_skill(market_service_t *service, const install_request_t *request,
                                 install_result_t **result);

/**
 * @brief 卸载 Agent
 * @param service 服务句柄
 * @param agent_id Agent ID
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_uninstall_agent(market_service_t *service, const char *agent_id);

/**
 * @brief 卸载 Skill
 * @param service 服务句柄
 * @param skill_id Skill ID
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_uninstall_skill(market_service_t *service, const char *skill_id);

/**
 * @brief 获取已安装的 Agent 列表
 * @param service 服务句柄
 * @param agents 输出参数，返回 Agent 信息数组
 * @param count 输出参数，返回 Agent 数量
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_get_installed_agents(market_service_t *service, agent_info_t ***agents,
                                        size_t *count);

/**
 * @brief 获取已安装的 Skill 列表
 * @param service 服务句柄
 * @param skills 输出参数，返回 Skill 信息数组
 * @param count 输出参数，返回 Skill 数量
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_get_installed_skills(market_service_t *service, skill_info_t ***skills,
                                        size_t *count);

/**
 * @brief 检查更新
 * @param service 服务句柄
 * @param id Agent 或 Skill ID
 * @param has_update 输出参数，返回是否有更新
 * @param latest_version 输出参数，返回最新版本
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_check_update(market_service_t *service, const char *id, bool *has_update,
                                char **latest_version);

/**
 * @brief 重载配置
 * @param service 服务句柄
 * @param manager 新的配置信息
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_reload_config(market_service_t *service, const market_config_t *manager);

/**
 * @brief 同步注册中心
 * @param service 服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int market_service_sync_registry(market_service_t *service);

#endif /* AIRY_RT_MARKET_SERVICE_H */
