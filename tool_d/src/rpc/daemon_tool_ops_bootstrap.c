// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_tool_ops_bootstrap.c
 * @brief Tool service / approval ops-table bootstrap implementation.
 */

#include "daemon_tool_ops_bootstrap.h"

#include "airy_tool_ops.h"

#include "tool_approval.h"
#include "tool_service.h"
#include "svc_logger.h"

static int g_tool_ops_initialized = 0;

/*
 * ARC-04: airy_tool_service-backed implementation of the atoms tool ops
 * table. The atoms layer (execution engine, orchestrator, tool adapter)
 * dispatches through this table without linking against daemons symbols
 * (ARC-02); the daemon layer owns the concrete implementation and injects
 * it here. Cleared on cleanup, after which atoms callers degrade
 * gracefully (BAN-319).
 *
 * The table storage (are_ops_set/get_tool) lives in the zero-dependency
 * small library airy_tool_ops, not in airy_tool_service itself, so that
 * atoms consumers resolve the accessor without an atoms->daemons edge;
 * the daemon links that small library PRIVATE (see tool_d/CMakeLists.txt).
 */
static const airy_tool_ops_t g_daemon_tool_ops = {
    /* Tool approval lifecycle (daemons: tool_approval.h) */
    .approval_create = tool_approval_create,
    .approval_destroy = tool_approval_destroy,
    .approval_check = tool_approval_check,

    /* Tool execution (daemons: tool_service.h) */
    .service_execute = tool_service_execute,
    .result_free = tool_result_free,
};

airy_err_t daemon_tool_ops_init(const char *daemon_name)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("daemon_tool_ops_init: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_tool_ops_initialized) {
        SVC_LOG_DEBUG("daemon_tool_ops_init: tool ops already injected (daemon=%s)", daemon_name);
        return AIRY_SUCCESS;
    }

    are_ops_set_tool(&g_daemon_tool_ops);
    g_tool_ops_initialized = 1;
    SVC_LOG_INFO("daemon_tool_ops_init: tool service/approval ops injected for '%s'", daemon_name);
    return AIRY_SUCCESS;
}

void daemon_tool_ops_cleanup(void)
{
    if (!g_tool_ops_initialized)
        return;

    are_ops_set_tool(NULL);
    g_tool_ops_initialized = 0;
    SVC_LOG_INFO("daemon_tool_ops_cleanup: tool service/approval ops detached");
}
