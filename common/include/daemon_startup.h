/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_startup.h
 * @brief Daemon startup-order orchestration and dependency DAG definitions.
 *
 * P1.23.1: defines the startup dependency DAG (directed acyclic graph) of
 * the 12 daemons.
 *
 * Startup layers (smaller Layer starts earlier):
 *
 *   Layer 0 - infrastructure (no daemon dependencies)
 *     monit_d, observe_d, info_d, notify_d
 *
 *   Layer 1 - core services
 *     sched_d   -> observe_d
 *     channel_d -> notify_d
 *
 *   Layer 2 - agent services
 *     llm_d    -> sched_d
 *     tool_d   -> llm_d, sched_d
 *     hook_d   -> tool_d
 *     plugin_d -> tool_d, hook_d
 *
 *   Layer 3 - business services
 *     market_d -> plugin_d
 *
 *   Layer 4 - gateway (depends on all services)
 *     gateway_d -> llm_d, tool_d, market_d
 *
 * DAG visualization:
 *
 *   monit_d ──┐
 *   observe_d ─┼──→ sched_d ──→ llm_d ──→ tool_d ──→ hook_d ──→ plugin_d ──→ market_d ──→ gateway_d
 *   info_d ────┤         └──────────────→ tool_d                      └──────────────→ gateway_d
 *   notify_d ──┼──→ channel_d                                               └──────────────→ gateway_d
 *              ┘
 */

#ifndef AIRY_RT_DAEMON_STARTUP_H
#define AIRY_RT_DAEMON_STARTUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define AIRY_DAEMON_COUNT 12
#define AIRY_MAX_DEPS_PER_DAEMON 4
#define AIRY_MAX_LAYERS 5


typedef enum {
    AIRY_DAEMON_MONIT = 0,
    AIRY_DAEMON_OBSERVE = 1,
    AIRY_DAEMON_INFO = 2,
    AIRY_DAEMON_NOTIFY = 3,
    AIRY_DAEMON_SCHED = 4,
    AIRY_DAEMON_CHANNEL = 5,
    AIRY_DAEMON_LLM = 6,
    AIRY_DAEMON_TOOL = 7,
    AIRY_DAEMON_HOOK = 8,
    AIRY_DAEMON_PLUGIN = 9,
    AIRY_DAEMON_MARKET = 10,
    AIRY_DAEMON_GATEWAY = 11,
    AIRY_DAEMON_INVALID = -1
} airy_daemon_id_t;


typedef struct {
    airy_daemon_id_t id;
    const char *name;
    const char *service_type;
    uint32_t layer;
    uint32_t health_timeout_ms;
    uint32_t health_interval_ms;
    uint16_t default_port;
    int dep_count;
    airy_daemon_id_t deps[AIRY_MAX_DEPS_PER_DAEMON];
} airy_daemon_desc_t;


/**
 * @brief Global daemon descriptor table, sorted by layer.
 *
 * The startup orchestrator walks this table; daemons in the same layer can
 * start in parallel, while crossing layers waits until all daemons of the
 * previous layer pass their health checks.
 */
static const airy_daemon_desc_t airy_daemon_table[AIRY_DAEMON_COUNT] = {

    [AIRY_DAEMON_MONIT] =
        {
            .id = AIRY_DAEMON_MONIT,
            .name = "monit_d",
            .service_type = "monitor",
            .layer = 0,
            .health_timeout_ms = 15000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 0,
            .deps = {0},
        },
    [AIRY_DAEMON_OBSERVE] =
        {
            .id = AIRY_DAEMON_OBSERVE,
            .name = "observe_d",
            .service_type = "observability",
            .layer = 0,
            .health_timeout_ms = 15000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 0,
            .deps = {0},
        },
    [AIRY_DAEMON_INFO] =
        {
            .id = AIRY_DAEMON_INFO,
            .name = "info_d",
            .service_type = "info",
            .layer = 0,
            .health_timeout_ms = 15000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 0,
            .deps = {0},
        },
    [AIRY_DAEMON_NOTIFY] =
        {
            .id = AIRY_DAEMON_NOTIFY,
            .name = "notify_d",
            .service_type = "notification",
            .layer = 0,
            .health_timeout_ms = 15000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 0,
            .deps = {0},
        },


    [AIRY_DAEMON_SCHED] =
        {
            .id = AIRY_DAEMON_SCHED,
            .name = "sched_d",
            .service_type = "scheduler",
            .layer = 1,
            .health_timeout_ms = 20000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 1,
            .deps = {AIRY_DAEMON_OBSERVE},
        },
    [AIRY_DAEMON_CHANNEL] =
        {
            .id = AIRY_DAEMON_CHANNEL,
            .name = "channel_d",
            .service_type = "channel",
            .layer = 1,
            .health_timeout_ms = 20000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 1,
            .deps = {AIRY_DAEMON_NOTIFY},
        },


    [AIRY_DAEMON_LLM] =
        {
            .id = AIRY_DAEMON_LLM,
            .name = "llm_d",
            .service_type = "llm",
            .layer = 2,
            .health_timeout_ms = 30000,
            .health_interval_ms = 1000,
            .default_port = 0,
            .dep_count = 1,
            .deps = {AIRY_DAEMON_SCHED},
        },
    [AIRY_DAEMON_TOOL] =
        {
            .id = AIRY_DAEMON_TOOL,
            .name = "tool_d",
            .service_type = "tool",
            .layer = 2,
            .health_timeout_ms = 30000,
            .health_interval_ms = 1000,
            .default_port = 8082,
            .dep_count = 2,
            .deps = {AIRY_DAEMON_LLM, AIRY_DAEMON_SCHED},
        },
    [AIRY_DAEMON_HOOK] =
        {
            .id = AIRY_DAEMON_HOOK,
            .name = "hook_d",
            .service_type = "hook",
            .layer = 2,
            .health_timeout_ms = 20000,
            .health_interval_ms = 500,
            .default_port = 0,
            .dep_count = 1,
            .deps = {AIRY_DAEMON_TOOL},
        },
    [AIRY_DAEMON_PLUGIN] =
        {
            .id = AIRY_DAEMON_PLUGIN,
            .name = "plugin_d",
            .service_type = "plugin",
            .layer = 2,
            .health_timeout_ms = 30000,
            .health_interval_ms = 1000,
            .default_port = 0,
            .dep_count = 2,
            .deps = {AIRY_DAEMON_TOOL, AIRY_DAEMON_HOOK},
        },


    [AIRY_DAEMON_MARKET] =
        {
            .id = AIRY_DAEMON_MARKET,
            .name = "market_d",
            .service_type = "marketplace",
            .layer = 3,
            .health_timeout_ms = 30000,
            .health_interval_ms = 1000,
            .default_port = 0,
            .dep_count = 1,
            .deps = {AIRY_DAEMON_PLUGIN},
        },


    [AIRY_DAEMON_GATEWAY] =
        {
            .id = AIRY_DAEMON_GATEWAY,
            .name = "gateway_d",
            .service_type = "gateway",
            .layer = 4,
            .health_timeout_ms = 30000,
            .health_interval_ms = 1000,
            .default_port = 8080,
            .dep_count = 3,
            .deps = {AIRY_DAEMON_LLM, AIRY_DAEMON_TOOL, AIRY_DAEMON_MARKET},
        },
};


/**
 * @brief Find a descriptor by daemon name.
 * @param name Daemon process name (e.g. "gateway_d")
 * @return Descriptor pointer, NULL if not found
 */
static inline const airy_daemon_desc_t *airy_daemon_find_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < AIRY_DAEMON_COUNT; i++) {
        if (airy_daemon_table[i].name && __builtin_strcmp(airy_daemon_table[i].name, name) == 0) {
            return &airy_daemon_table[i];
        }
    }
    return NULL;
}

/** @brief Count daemons in the given layer. */
static inline int airy_daemon_count_in_layer(uint32_t layer)
{
    int count = 0;
    for (int i = 0; i < AIRY_DAEMON_COUNT; i++) {
        if (airy_daemon_table[i].layer == layer)
            count++;
    }
    return count;
}

/** @brief Get the maximum layer value. */
static inline uint32_t airy_daemon_max_layer(void)
{
    uint32_t max_l = 0;
    for (int i = 0; i < AIRY_DAEMON_COUNT; i++) {
        if (airy_daemon_table[i].layer > max_l)
            max_l = airy_daemon_table[i].layer;
    }
    return max_l;
}

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_STARTUP_H */
