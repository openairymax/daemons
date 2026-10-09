/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.12.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_think_d.h"

#include "daemon_main.h"
#include "daemon_security_dome.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_llm_ops_bootstrap.h"
#include "daemon_tool_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(think_d, think,
                      THINK_D_SOCKET_UNIX, THINK_D_SOCKET_WIN,
                      THINK_D_TCP_PORT, THINK_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(think_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_THINK_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
    { daemon_llm_ops_init, daemon_llm_ops_cleanup },
    { daemon_tool_ops_init, daemon_tool_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "think_d",
        .cname = "think",
        .env_debug = "AIRY_THINK_D_DEBUG",
        .sd_type = "think",
        .tags = "think,core",
        .method_total = 9,
        .running_lock = &g_running_lock_think_d,
        .signal_handler = signal_handler_think_d,
        .log_toggle = svc_log_toggle_handler_think_d,
        .print_usage = print_usage_think_d,
        .on_client = daemon_on_client_think_d,
        .dispatcher = &g_dispatcher_think_d,
        .event_driver = &g_event_driver_think_d,
        .bsd = &g_bsd_think_d,
        .pool_max_events = 16,
        .pool_min = 2,
        .pool_max = 4,
        .pool_queue = 32,
        .concurrent_clients = 1,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, svc_activate, daemon_dome_init_pep, daemon_dome_cleanup),
    };
    return daemon_boot(argc, argv, &boot);
}
