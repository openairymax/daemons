/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.10.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_agent_d.h"

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(agent_d, agent,
                      AGENT_D_SOCKET_UNIX, AGENT_D_SOCKET_WIN,
                      AGENT_D_TCP_PORT, AGENT_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(agent_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_AGENT_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "agent_d",
        .cname = "agent",
        .env_debug = "AIRY_AGENT_D_DEBUG",
        .sd_type = "agent",
        .tags = "agent,core",
        .method_total = 13,
        .running_lock = &g_running_lock_agent_d,
        .signal_handler = signal_handler_agent_d,
        .log_toggle = svc_log_toggle_handler_agent_d,
        .print_usage = print_usage_agent_d,
        .on_client = daemon_on_client_agent_d,
        .dispatcher = &g_dispatcher_agent_d,
        .event_driver = &g_event_driver_agent_d,
        .bsd = &g_bsd_agent_d,
        .bipc = &g_bipc_agent_d,
        .pool_max_events = 256,
        .pool_min = 16,
        .pool_max = 128,
        .pool_queue = 4096,
        .concurrent_clients = 1,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, svc_activate, daemon_cupolas_init_pep),
    };
    return daemon_boot(argc, argv, &boot);
}
