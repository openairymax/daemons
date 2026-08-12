// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file skill_registry.c
 * @brief Skill registration-management module.
 * @details Implements skill registration, query and management on top of
 *          the market_service public API.
 */

#include "daemon_errors.h"
#include "market_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int skill_registry_register(market_service_t *service, const skill_info_t *skill_info)
{
    if (!service || !skill_info) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_register_skill(service, skill_info);
}

skill_info_t *skill_registry_find(market_service_t *service, const char *skill_id)
{
    if (!service || !skill_id) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    search_params_t params = {0};
    params.query = (char *)skill_id;
    params.limit = 1;

    skill_info_t **results = NULL;
    size_t count = 0;
    int ret = market_service_search_skills(service, &params, &results, &count);
    if (ret != 0 || count == 0 || !results) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
    }

    skill_info_t *found = results[0];
    AIRY_FREE(results);
    return found;
}

int skill_registry_remove(market_service_t *service, const char *skill_id)
{
    if (!service || !skill_id) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_uninstall_skill(service, skill_id);
}

int skill_registry_list(market_service_t *service, skill_info_t ***skills, size_t *count)
{
    if (!service || !skills || !count) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_get_installed_skills(service, skills, count);
}
