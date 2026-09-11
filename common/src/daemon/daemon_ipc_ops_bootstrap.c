// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_ipc_ops_bootstrap.c
 * @brief Unified IPC/RPC/ServiceDiscovery ops-table bootstrap implementation.
 */

#include "daemon_ipc_ops_bootstrap.h"

#include "airy_ipc_ops.h"

#include "daemon_bootstrap_ipc.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_rpc_client.h"
#include "ipc_bus_helper.h"
#include "ipc_service_bus.h"
#include "service_discovery_helper.h"
#include "svc_logger.h"

static int g_ipc_ops_initialized = 0;

/*
 * ARC-04: svc_common-backed implementation of the atoms IPC ops table.
 * The atoms layer (coreloopthree IPC adapters, orchestrator, language
 * gateway) dispatches through this table without linking against
 * svc_common (ARC-02); the daemon layer owns the concrete implementation
 * and injects it here. Cleared on cleanup, after which atoms callers
 * degrade gracefully (BAN-319).
 *
 * The table storage (are_ops_set/get_ipc) lives in the zero-dependency
 * small library airy_ipc_ops, not in svc_common itself, to avoid the
 * svc_common -> svc_common self-reference that a same-library definition
 * would create for atoms consumers.
 */
static const airy_ipc_ops_t g_daemon_ipc_ops = {
    /* IPC Bus bootstrap (commons: daemon_bootstrap_ipc.h) */
    .bootstrap_ipc_start = daemon_bootstrap_ipc_start,
    .bootstrap_ipc_stop = daemon_bootstrap_ipc_stop,
    .bootstrap_ipc_get_helper = daemon_bootstrap_ipc_get_helper,
    .bootstrap_ipc_is_running = daemon_bootstrap_ipc_is_running,

    /* ServiceDiscovery bootstrap (commons: daemon_bootstrap_sd.h) */
    .bootstrap_sd_stop = daemon_bootstrap_sd_stop,
    .bootstrap_sd_get_helper = daemon_bootstrap_sd_get_helper,
    .sd_select_with_strategy = sd_helper_select_with_strategy,

    /* Unix-socket JSON-RPC client (commons: daemon_rpc_client.h) */
    .rpc_call = daemon_rpc_call,
    .rpc_call_cancelable = daemon_rpc_call_cancelable,
    .rpc_call_stream = daemon_rpc_call_stream,

    /* IPC Bus helper message path (commons: ipc_bus_helper.h) */
    .bus_helper_request = ipc_bus_helper_request,
    .bus_message_create = ipc_bus_message_create,
    .bus_message_free = ipc_bus_message_free,
};

airy_err_t daemon_ipc_ops_init(const char *daemon_name)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("daemon_ipc_ops_init: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_ipc_ops_initialized) {
        SVC_LOG_DEBUG("daemon_ipc_ops_init: IPC ops already injected (daemon=%s)", daemon_name);
        return AIRY_SUCCESS;
    }

    are_ops_set_ipc(&g_daemon_ipc_ops);
    g_ipc_ops_initialized = 1;
    SVC_LOG_INFO("daemon_ipc_ops_init: IPC/RPC/ServiceDiscovery ops injected for '%s'",
                 daemon_name);
    return AIRY_SUCCESS;
}

void daemon_ipc_ops_cleanup(void)
{
    if (!g_ipc_ops_initialized)
        return;

    are_ops_set_ipc(NULL);
    g_ipc_ops_initialized = 0;
    SVC_LOG_INFO("daemon_ipc_ops_cleanup: IPC/RPC/ServiceDiscovery ops detached");
}
