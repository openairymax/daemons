/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_startup.h
 * @brief daemon 启动顺序编排与依赖 DAG 定义
 *
 * P1.23.1: 定义 12 daemon 的启动依赖 DAG（有向无环图）。
 *
 * 启动层级（Layer 越小越先启动）：
 *
 *   Layer 0 — 基础设施（无 daemon 依赖）
 *     monit_d, observe_d, info_d, notify_d
 *
 *   Layer 1 — 核心服务
 *     sched_d   → observe_d
 *     channel_d → notify_d
 *
 *   Layer 2 — Agent 服务
 *     llm_d    → sched_d
 *     tool_d   → llm_d, sched_d
 *     hook_d   → tool_d
 *     plugin_d → tool_d, hook_d
 *
 *   Layer 3 — 业务服务
 *     market_d → plugin_d
 *
 *   Layer 4 — 网关（依赖所有服务）
 *     gateway_d → llm_d, tool_d, market_d
 *
 * DAG 可视化：
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
 * @brief 全局 daemon 描述表 — 按 layer 排序
 *
 * 启动编排器遍历此表，同 layer 内可并行启动，
 * 跨 layer 必须等待前一层所有 daemon 健康检查通过。
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
 * @brief 根据 daemon 名称查找描述
 * @param name daemon 进程名 (e.g. "gateway_d")
 * @return 描述指针，未找到返回 NULL
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

/**
 * @brief 获取指定 layer 的 daemon 数量
 */
static inline int airy_daemon_count_in_layer(uint32_t layer)
{
    int count = 0;
    for (int i = 0; i < AIRY_DAEMON_COUNT; i++) {
        if (airy_daemon_table[i].layer == layer)
            count++;
    }
    return count;
}

/**
 * @brief 获取最大 layer 值
 */
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
