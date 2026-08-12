/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file market_service_internal.h
 * @brief Internal cross-file shared declarations of the market service
 *        (shared by the domains after market_service_impl.c split).
 */

#ifndef AIRY_RT_DAEMON_MARKET_D_MARKET_SERVICE_INTERNAL_H
#define AIRY_RT_DAEMON_MARKET_D_MARKET_SERVICE_INTERNAL_H

#include "market_service.h"
#include "platform.h"
#include <airymax/lsm_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SKILLS 256

struct market_service {
    market_config_t config;
    agent_info_t *agents[AIRY_CAP_MAX_AGENTS];
    size_t agent_count;
    skill_info_t *skills[MAX_SKILLS];
    size_t skill_count;
    airy_mtx_t lock;
    int initialized;
};

#ifdef _WIN32
int win_run_command(const char *prog, const char *const args[]);
#endif

int recursive_remove(const char *path);
int is_safe_for_shell(const char *str);
int is_valid_url(const char *url);
int is_safe_path_component(const char *str);
const char *market_default_storage(const char *subdir);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MARKET_D_MARKET_SERVICE_INTERNAL_H */
