// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_listing.c
 * @brief 市场服务列表域：已安装 agent/skill 列表查询与更新检查
 */

#include "airy_memory.h"
#include "error.h"
#include "market_service.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#include "market_service_internal.h"

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
