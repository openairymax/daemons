// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file daemon_state.c
 * @brief Daemon 运行面形式化状态机实现（迁移全表见 daemon_state.h）。
 */

#include "daemon_state.h"

dmn_state_t dmn_state_next(dmn_state_t cur, bool self_ok, bool deps_ok, bool confirmed)
{
    switch (cur) {
    case DMN_ST_NOMINAL:
        if (!self_ok)
            return DMN_ST_UNAVAILABLE;
        return deps_ok ? DMN_ST_NOMINAL : DMN_ST_DEGRADED;
    case DMN_ST_DEGRADED:
        if (!self_ok)
            return DMN_ST_UNAVAILABLE;
        return deps_ok ? DMN_ST_NOMINAL : DMN_ST_DEGRADED;
    case DMN_ST_UNAVAILABLE:
        if (!self_ok)
            return DMN_ST_UNAVAILABLE;
        if (!deps_ok)
            return DMN_ST_DEGRADED;
        return confirmed ? DMN_ST_NOMINAL : DMN_ST_RECOVERING;
    case DMN_ST_RECOVERING:
        if (!self_ok)
            return DMN_ST_UNAVAILABLE;
        if (!deps_ok)
            return DMN_ST_DEGRADED;
        return confirmed ? DMN_ST_NOMINAL : DMN_ST_RECOVERING;
    default:
        return DMN_ST_UNAVAILABLE; /* 非法状态 fail-closed */
    }
}

const char *dmn_state_name(dmn_state_t st)
{
    switch (st) {
    case DMN_ST_NOMINAL:
        return "nominal";
    case DMN_ST_DEGRADED:
        return "degraded";
    case DMN_ST_RECOVERING:
        return "recovering";
    default:
        return "unavailable";
    }
}
