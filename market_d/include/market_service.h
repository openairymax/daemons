/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file market_service.h
 * @brief Market service interface.
 * @details Handles registration, discovery, installation and management of
 *          agents and skills.
 */

#ifndef AIRY_RT_MARKET_SERVICE_H
#define AIRY_RT_MARKET_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Market service config. */
typedef struct {
    char *registry_url;
    char *storage_path;
    uint32_t sync_interval_ms;
    uint32_t cache_ttl_ms;
    bool enable_remote_registry;
    bool enable_auto_update;
} market_config_t;

/** @brief Agent type. */
typedef enum {
    AGENT_TYPE_ASSISTANT,
    AGENT_TYPE_EXPERT,
    AGENT_TYPE_SPECIALIZED,
    AGENT_TYPE_CUSTOM,
    AGENT_TYPE_COUNT
} agent_type_t;

/** @brief Agent status. */
typedef enum {
    AGENT_STATUS_AVAILABLE,
    AGENT_STATUS_INSTALLING,
    AGENT_STATUS_ERROR,
    AGENT_STATUS_DISABLED,
    AGENT_STATUS_COUNT
} agent_status_t;

/** @brief Skill type. */
typedef enum {
    SKILL_TYPE_TOOL,
    SKILL_TYPE_KNOWLEDGE,
    SKILL_TYPE_INTEGRATION,
    SKILL_TYPE_CUSTOM,
    SKILL_TYPE_COUNT
} skill_type_t;

/** @brief Agent info. */
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

/** @brief Skill info. */
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

/** @brief Install request. */
typedef struct {
    char *id;
    char *version;
    bool force_update;
    char *install_path;
} install_request_t;

/** @brief Install result. */
typedef struct {
    bool success;
    char *message;
    char *installed_version;
    char *install_path;
    int error_code;
} install_result_t;

/** @brief Search parameters. */
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

/** @brief Market service handle. */
typedef struct market_service market_service_t;

/**
 * @brief Create a market service.
 * @param manager Config info
 * @param service Output parameter, returns the created service handle
 * @return 0 on success, non-zero error code
 */
int market_service_create(const market_config_t *manager, market_service_t **service);

/**
 * @brief Destroy a market service.
 * @param service Service handle
 * @return 0 on success, non-zero error code
 */
int market_service_destroy(market_service_t *service);

/**
 * @brief Register an agent.
 * @param service Service handle
 * @param agent_info Agent info
 * @return 0 on success, non-zero error code
 */
int market_service_register_agent(market_service_t *service, const agent_info_t *agent_info);

/**
 * @brief Register a skill.
 * @param service Service handle
 * @param skill_info Skill info
 * @return 0 on success, non-zero error code
 */
int market_service_register_skill(market_service_t *service, const skill_info_t *skill_info);

/**
 * @brief Search agents.
 * @param service Service handle
 * @param params Search parameters
 * @param agents Output parameter, returns the agent-info array
 * @param count Output parameter, returns the agent count
 * @return 0 on success, non-zero error code
 */
int market_service_search_agents(market_service_t *service, const search_params_t *params,
                                 agent_info_t ***agents, size_t *count);

/**
 * @brief Search skills.
 * @param service Service handle
 * @param params Search parameters
 * @param skills Output parameter, returns the skill-info array
 * @param count Output parameter, returns the skill count
 * @return 0 on success, non-zero error code
 */
int market_service_search_skills(market_service_t *service, const search_params_t *params,
                                 skill_info_t ***skills, size_t *count);

/**
 * @brief Install an agent.
 * @param service Service handle
 * @param request Install request
 * @param result Output parameter, returns the install result
 * @return 0 on success, non-zero error code
 */
int market_service_install_agent(market_service_t *service, const install_request_t *request,
                                 install_result_t **result);

/**
 * @brief Install a skill.
 * @param service Service handle
 * @param request Install request
 * @param result Output parameter, returns the install result
 * @return 0 on success, non-zero error code
 */
int market_service_install_skill(market_service_t *service, const install_request_t *request,
                                 install_result_t **result);

/**
 * @brief Uninstall an agent.
 * @param service Service handle
 * @param agent_id Agent ID
 * @return 0 on success, non-zero error code
 */
int market_service_uninstall_agent(market_service_t *service, const char *agent_id);

/**
 * @brief Uninstall a skill.
 * @param service Service handle
 * @param skill_id Skill ID
 * @return 0 on success, non-zero error code
 */
int market_service_uninstall_skill(market_service_t *service, const char *skill_id);

/**
 * @brief Get the installed-agent list.
 * @param service Service handle
 * @param agents Output parameter, returns the agent-info array
 * @param count Output parameter, returns the agent count
 * @return 0 on success, non-zero error code
 */
int market_service_get_installed_agents(market_service_t *service, agent_info_t ***agents,
                                        size_t *count);

/**
 * @brief Get the installed-skill list.
 * @param service Service handle
 * @param skills Output parameter, returns the skill-info array
 * @param count Output parameter, returns the skill count
 * @return 0 on success, non-zero error code
 */
int market_service_get_installed_skills(market_service_t *service, skill_info_t ***skills,
                                        size_t *count);

/**
 * @brief Check for updates.
 * @param service Service handle
 * @param id Agent or Skill ID
 * @param has_update Output parameter, whether an update exists
 * @param latest_version Output parameter, returns the latest version
 * @return 0 on success, non-zero error code
 */
int market_service_check_update(market_service_t *service, const char *id, bool *has_update,
                                char **latest_version);

/**
 * @brief Reload the config.
 * @param service Service handle
 * @param manager New config info
 * @return 0 on success, non-zero error code
 */
int market_service_reload_config(market_service_t *service, const market_config_t *manager);

/**
 * @brief Sync with the registry.
 * @param service Service handle
 * @return 0 on success, non-zero error code
 */
int market_service_sync_registry(market_service_t *service);

#endif /* AIRY_RT_MARKET_SERVICE_H */
