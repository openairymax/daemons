// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file publisher.c
 * @brief Publish-management module.
 * @details Implements publish and update on top of the market_service
 *          public API.
 */

#include "market_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

int publisher_publish_agent(market_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_register_agent(service, agent_info);
}

int publisher_publish_skill(market_service_t *service, const skill_info_t *skill_info)
{
    if (!service || !skill_info) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_register_skill(service, skill_info);
}

int publisher_check_agent_update(market_service_t *service, const char *agent_id, bool *has_update,
                                 char **latest_version)
{
    if (!service || !agent_id || !has_update || !latest_version) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_check_update(service, agent_id, has_update, latest_version);
}

int publisher_check_skill_update(market_service_t *service, const char *skill_id, bool *has_update,
                                 char **latest_version)
{
    if (!service || !skill_id || !has_update || !latest_version) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_check_update(service, skill_id, has_update, latest_version);
}

int publisher_sync_to_registry(market_service_t *service)
{
    if (!service) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_sync_registry(service);
}
