// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file installer.c
 * @brief Installation-management module.
 * @details Implements install and uninstall on top of the market_service
 *          public API.
 */

#include "market_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "error.h"

static int __attribute__((unused)) create_directory(const char *path)
{
#ifdef _WIN32
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static bool __attribute__((unused)) directory_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int installer_install_agent(market_service_t *service, const install_request_t *request,
                            install_result_t **result)
{
    if (!service || !request || !result) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_install_agent(service, request, result);
}

int installer_install_skill(market_service_t *service, const install_request_t *request,
                            install_result_t **result)
{
    if (!service || !request || !result) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_install_skill(service, request, result);
}

int installer_uninstall_agent(market_service_t *service, const char *agent_id)
{
    if (!service || !agent_id) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_uninstall_agent(service, agent_id);
}

int installer_uninstall_skill(market_service_t *service, const char *skill_id)
{
    if (!service || !skill_id) {
        return AIRY_ERR_INVALID_PARAM;
    }
    return market_service_uninstall_skill(service, skill_id);
}
