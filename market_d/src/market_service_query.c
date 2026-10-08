// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_query.c
 * @brief Market 只读查询域：关键字检索 + 已安装清单 + 更新检查。
 *
 * 检索与清单两族函数共享同一 doubling-realloc 结果数组增长模式与只读
 * 加锁语义，0.1.19 §248 并原 market_service_search.c /
 * market_service_listing.c 两源件为单一查询域编译单元；全部函数仅持
 * service->lock 读快照，不改变服务状态。
 */

#include "airy_memory.h"
#include "error.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#include "market_service_internal.h"

int market_service_search_agents(market_service_t *service, const search_params_t *params,
                                 agent_info_t ***agents, size_t *count)
{
    if (!service || !params || !agents || !count || !service->initialized) {
        SVC_LOG_ERROR("market_service_search_agents: NULL parameter or not initialized "
                      "(service=%p, params=%p, agents=%p, count=%p, initialized=%d)",
                      (const void *)service, (const void *)params, (const void *)agents,
                      (const void *)count, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    size_t results_size = 16;
    agent_info_t **results = (agent_info_t **)AIRY_MALLOC(sizeof(agent_info_t *) * results_size);
    if (!results) {
        SVC_LOG_ERROR("market_service_search_agents: malloc failed for search results");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate search results");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (params->query && strlen(params->query) > 0) {
            if (!strstr(service->agents[i]->agent_id, params->query) &&
                !strstr(service->agents[i]->name, params->query) &&
                !(service->agents[i]->description &&
                  strstr(service->agents[i]->description, params->query))) {
                continue;
            }
        }

        if (found >= results_size) {
            /* Doubling overflow check: results_size * 2 * sizeof(agent_info_t *)
             * must not wrap; on overflow stop growing and return the partial
             * results collected so far */
            if (results_size > SIZE_MAX / (2 * sizeof(agent_info_t *)))
                break;
            results_size *= 2;
            agent_info_t **tmp =
                (agent_info_t **)AIRY_REALLOC(results, sizeof(agent_info_t *) * results_size);
            if (!tmp) {
                SVC_LOG_ERROR("market_service_search_agents: realloc failed for search results "
                              "(results_size=%zu)",
                              results_size);
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize search results");
            }
            results = tmp;
        }

        results[found++] = service->agents[i];
        if (params->limit > 0 && found >= params->limit)
            break;
    }
    airy_mtx_unlock(&service->lock);

    *agents = results;
    *count = found;
    return 0;
}

int market_service_search_skills(market_service_t *service, const search_params_t *params,
                                 skill_info_t ***skills, size_t *count)
{
    if (!service || !params || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    skill_info_t **results = (skill_info_t **)AIRY_MALLOC(sizeof(skill_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill search results");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (params->query && strlen(params->query) > 0) {
            if (!strstr(service->skills[i]->skill_id, params->query) &&
                !strstr(service->skills[i]->name, params->query) &&
                !(service->skills[i]->description &&
                  strstr(service->skills[i]->description, params->query))) {
                continue;
            }
        }

        if (found >= results_size) {
            /* Doubling overflow check: results_size * 2 * sizeof(skill_info_t *)
             * must not wrap; on overflow stop growing and return the partial
             * results collected so far */
            if (results_size > SIZE_MAX / (2 * sizeof(skill_info_t *)))
                break;
            results_size *= 2;
            skill_info_t **tmp =
                (skill_info_t **)AIRY_REALLOC(results, sizeof(skill_info_t *) * results_size);
            if (!tmp) {
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize skill search results");
            }
            results = tmp;
        }

        results[found++] = service->skills[i];
        if (params->limit > 0 && found >= params->limit)
            break;
    }
    airy_mtx_unlock(&service->lock);

    *skills = results;
    *count = found;
    return 0;
}

int market_service_get_installed_agents(market_service_t *service, agent_info_t ***agents,
                                        size_t *count)
{
    if (!service || !agents || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    agent_info_t **results = (agent_info_t **)AIRY_MALLOC(sizeof(agent_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate installed agents list");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]->status == AGENT_STATUS_AVAILABLE ||
            service->agents[i]->status == AGENT_STATUS_ERROR) {

            if (found >= results_size) {
                /* Doubling overflow check: results_size * 2 *
                 * sizeof(agent_info_t *) must not wrap; on overflow stop
                 * growing and return the partial results collected so far */
                if (results_size > SIZE_MAX / (2 * sizeof(agent_info_t *)))
                    break;
                results_size *= 2;
                agent_info_t **tmp =
                    (agent_info_t **)AIRY_REALLOC(results, sizeof(agent_info_t *) * results_size);
                if (!tmp) {
                    AIRY_FREE(results);
                    airy_mtx_unlock(&service->lock);
                    AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize installed agents list");
                }
                results = tmp;
            }

            results[found++] = service->agents[i];
        }
    }
    airy_mtx_unlock(&service->lock);

    *agents = results;
    *count = found;
    return 0;
}

int market_service_get_installed_skills(market_service_t *service, skill_info_t ***skills,
                                        size_t *count)
{
    if (!service || !skills || !count || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    size_t results_size = 16;
    skill_info_t **results = (skill_info_t **)AIRY_MALLOC(sizeof(skill_info_t *) * results_size);
    if (!results) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate installed skills list");
    }

    size_t found = 0;
    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (found >= results_size) {
            /* Doubling overflow check: results_size * 2 * sizeof(skill_info_t *)
             * must not wrap; on overflow stop growing and return the partial
             * results collected so far */
            if (results_size > SIZE_MAX / (2 * sizeof(skill_info_t *)))
                break;
            results_size *= 2;
            skill_info_t **tmp =
                (skill_info_t **)AIRY_REALLOC(results, sizeof(skill_info_t *) * results_size);
            if (!tmp) {
                AIRY_FREE(results);
                airy_mtx_unlock(&service->lock);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to resize installed skills list");
            }
            results = tmp;
        }

        results[found++] = service->skills[i];
    }
    airy_mtx_unlock(&service->lock);

    *skills = results;
    *count = found;
    return 0;
}

int market_service_check_update(market_service_t *service, const char *id, bool *has_update,
                                char **latest_version)
{
    if (!service || !id || !has_update || !latest_version || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    *has_update = false;

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, id) == 0) {
            *latest_version = AIRY_STRDUP(service->agents[i]->version);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, id) == 0) {
            *latest_version = AIRY_STRDUP(service->skills[i]->version);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    *latest_version = NULL;
    airy_mtx_unlock(&service->lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "update check: id not found");
}
