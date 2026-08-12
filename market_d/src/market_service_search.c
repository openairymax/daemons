// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_search.c
 * @brief 市场服务搜索域：按查询关键字检索 agent/skill，结果数组动态扩容
 */

#include "airy_memory.h"
#include "error.h"
#include "market_service.h"
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
