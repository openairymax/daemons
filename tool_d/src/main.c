/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.9.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_tool_d.h"

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_llm_ops_bootstrap.h"
#include "daemon_tool_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(tool_d, tool,
                      TOOL_D_SOCKET_UNIX, TOOL_D_SOCKET_WIN,
                      TOOL_D_TCP_PORT, TOOL_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(tool_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_TOOL_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
    { daemon_llm_ops_init, daemon_llm_ops_cleanup },
    { daemon_tool_ops_init, daemon_tool_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "tool_d",
        .cname = "tool",
        .env_debug = "AIRY_TOOL_D_DEBUG",
        .sd_type = "tool",
        .tags = "tool,core",
        .method_total = 11,
        .running_lock = &g_running_lock_tool_d,
        .signal_handler = signal_handler_tool_d,
        .log_toggle = svc_log_toggle_handler_tool_d,
        .print_usage = print_usage_tool_d,
        .on_client = daemon_on_client_tool_d,
        .dispatcher = &g_dispatcher_tool_d,
        .event_driver = &g_event_driver_tool_d,
        .bsd = &g_bsd_tool_d,
        .bipc = &g_bipc_tool_d,
        .pool_max_events = 64,
        .pool_min = 4,
        .pool_max = 8,
        .pool_queue = 256,
        .concurrent_clients = 1,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, daemon_cupolas_init_pep),
    };
    return daemon_boot(argc, argv, &boot);
}
