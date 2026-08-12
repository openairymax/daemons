// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_registry.c
 * @brief Market service registration domain: agent/skill registration
 *        (including same-name update and deep field copy).
 */

#include "airy_memory.h"
#include "error.h"
#include "market_service.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "market_service_internal.h"

int market_service_register_agent(market_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("market_service_register_agent: NULL parameter or not initialized "
                      "(service=%p, agent_info=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (service->agent_count >= AIRY_CAP_MAX_AGENTS) {
        SVC_LOG_ERROR("market_service_register_agent: max agents exceeded (count=%zu, max=%d)",
                      service->agent_count, AIRY_CAP_MAX_AGENTS);
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "max agents exceeded");
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            AIRY_FREE(service->agents[i]->name);
            service->agents[i]->name = NULL;
            AIRY_FREE(service->agents[i]->version);
            service->agents[i]->version = NULL;
            AIRY_FREE(service->agents[i]->description);
            service->agents[i]->description = NULL;
            AIRY_FREE(service->agents[i]->author);
            service->agents[i]->author = NULL;
            AIRY_FREE(service->agents[i]->repository);
            service->agents[i]->repository = NULL;
            AIRY_FREE(service->agents[i]->dependencies);
            service->agents[i]->dependencies = NULL;

            service->agents[i]->name = agent_info->name ? AIRY_STRDUP(agent_info->name) : NULL;
            service->agents[i]->version =
                agent_info->version ? AIRY_STRDUP(agent_info->version) : NULL;
            service->agents[i]->description =
                agent_info->description ? AIRY_STRDUP(agent_info->description) : NULL;
            service->agents[i]->type = agent_info->type;
            service->agents[i]->status = agent_info->status;
            service->agents[i]->author =
                agent_info->author ? AIRY_STRDUP(agent_info->author) : NULL;
            service->agents[i]->repository =
                agent_info->repository ? AIRY_STRDUP(agent_info->repository) : NULL;
            service->agents[i]->dependencies =
                agent_info->dependencies ? AIRY_STRDUP(agent_info->dependencies) : NULL;
            service->agents[i]->rating = agent_info->rating;
            service->agents[i]->download_count = agent_info->download_count;
            service->agents[i]->last_updated = (uint64_t)time(NULL);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
    if (!new_agent) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("market_service_register_agent: calloc failed for new agent entry");
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate agent entry");
    }

    new_agent->agent_id = agent_info->agent_id ? AIRY_STRDUP(agent_info->agent_id) : NULL;
    new_agent->name = agent_info->name ? AIRY_STRDUP(agent_info->name) : NULL;
    new_agent->version = agent_info->version ? AIRY_STRDUP(agent_info->version) : NULL;
    new_agent->description = agent_info->description ? AIRY_STRDUP(agent_info->description) : NULL;
    new_agent->type = agent_info->type;
    new_agent->status = agent_info->status;
    new_agent->author = agent_info->author ? AIRY_STRDUP(agent_info->author) : NULL;
    new_agent->repository = agent_info->repository ? AIRY_STRDUP(agent_info->repository) : NULL;
    new_agent->dependencies =
        agent_info->dependencies ? AIRY_STRDUP(agent_info->dependencies) : NULL;
    if (!new_agent->agent_id || !new_agent->name || !new_agent->version) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("market_service_register_agent: strdup failed for required agent fields "
                      "(agent_id=%p, name=%p, version=%p)",
                      (const void *)new_agent->agent_id, (const void *)new_agent->name,
                      (const void *)new_agent->version);
        AIRY_FREE(new_agent->agent_id);
        AIRY_FREE(new_agent->name);
        AIRY_FREE(new_agent->version);
        AIRY_FREE(new_agent->description);
        AIRY_FREE(new_agent->author);
        AIRY_FREE(new_agent->repository);
        AIRY_FREE(new_agent->dependencies);
        AIRY_FREE(new_agent);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate agent required fields");
    }
    new_agent->rating = agent_info->rating;
    new_agent->download_count = agent_info->download_count;
    new_agent->last_updated = (uint64_t)time(NULL);

    service->agents[service->agent_count++] = new_agent;
    airy_mtx_unlock(&service->lock);
    return 0;
}

int market_service_register_skill(market_service_t *service, const skill_info_t *skill_info)
{
    if (!service || !skill_info || !service->initialized) {
        SVC_LOG_ERROR("market_service_register_skill: NULL parameter or not initialized "
                      "(service=%p, skill_info=%p, initialized=%d)",
                      (const void *)service, (const void *)skill_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (service->skill_count >= MAX_SKILLS) {
        SVC_LOG_ERROR("market_service_register_skill: max skills exceeded (count=%zu, max=%d)",
                      service->skill_count, MAX_SKILLS);
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "max skills exceeded");
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->skill_count; i++) {
        if (strcmp(service->skills[i]->skill_id, skill_info->skill_id) == 0) {
            AIRY_FREE(service->skills[i]->name);
            service->skills[i]->name = NULL;
            AIRY_FREE(service->skills[i]->version);
            service->skills[i]->version = NULL;
            AIRY_FREE(service->skills[i]->description);
            service->skills[i]->description = NULL;
            AIRY_FREE(service->skills[i]->author);
            service->skills[i]->author = NULL;
            AIRY_FREE(service->skills[i]->repository);
            service->skills[i]->repository = NULL;
            AIRY_FREE(service->skills[i]->dependencies);
            service->skills[i]->dependencies = NULL;

            service->skills[i]->name = skill_info->name ? AIRY_STRDUP(skill_info->name) : NULL;
            service->skills[i]->version =
                skill_info->version ? AIRY_STRDUP(skill_info->version) : NULL;
            service->skills[i]->description =
                skill_info->description ? AIRY_STRDUP(skill_info->description) : NULL;
            service->skills[i]->type = skill_info->type;
            service->skills[i]->author =
                skill_info->author ? AIRY_STRDUP(skill_info->author) : NULL;
            service->skills[i]->repository =
                skill_info->repository ? AIRY_STRDUP(skill_info->repository) : NULL;
            service->skills[i]->dependencies =
                skill_info->dependencies ? AIRY_STRDUP(skill_info->dependencies) : NULL;
            service->skills[i]->rating = skill_info->rating;
            service->skills[i]->download_count = skill_info->download_count;
            service->skills[i]->last_updated = (uint64_t)time(NULL);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    skill_info_t *new_skill = (skill_info_t *)AIRY_CALLOC(1, sizeof(skill_info_t));
    if (!new_skill) {
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate skill entry");
    }

    new_skill->skill_id = skill_info->skill_id ? AIRY_STRDUP(skill_info->skill_id) : NULL;
    new_skill->name = skill_info->name ? AIRY_STRDUP(skill_info->name) : NULL;
    new_skill->version = skill_info->version ? AIRY_STRDUP(skill_info->version) : NULL;
    new_skill->description = skill_info->description ? AIRY_STRDUP(skill_info->description) : NULL;
    new_skill->type = skill_info->type;
    new_skill->author = skill_info->author ? AIRY_STRDUP(skill_info->author) : NULL;
    new_skill->repository = skill_info->repository ? AIRY_STRDUP(skill_info->repository) : NULL;
    new_skill->dependencies =
        skill_info->dependencies ? AIRY_STRDUP(skill_info->dependencies) : NULL;
    if (!new_skill->skill_id || !new_skill->name || !new_skill->version) {
        AIRY_FREE(new_skill->skill_id);
        AIRY_FREE(new_skill->name);
        AIRY_FREE(new_skill->version);
        AIRY_FREE(new_skill->description);
        AIRY_FREE(new_skill->author);
        AIRY_FREE(new_skill->repository);
        AIRY_FREE(new_skill->dependencies);
        AIRY_FREE(new_skill);
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate skill required fields");
    }
    new_skill->rating = skill_info->rating;
    new_skill->download_count = skill_info->download_count;
    new_skill->last_updated = (uint64_t)time(NULL);

    service->skills[service->skill_count++] = new_skill;
    airy_mtx_unlock(&service->lock);
    return 0;
}
