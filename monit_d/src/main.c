/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.10.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_monit_d.h"

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(monit_d, monitor,
                      MONIT_D_SOCKET_UNIX, MONIT_D_SOCKET_WIN,
                      MONIT_D_TCP_PORT, MONIT_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(monit_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_MONIT_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "monit_d",
        .cname = "monitor",
        .env_debug = "AIRY_MONIT_D_DEBUG",
        .sd_type = "monitor",
        .tags = "monitor,core",
        .method_total = 12,
        .running_lock = &g_running_lock_monit_d,
        .signal_handler = signal_handler_monit_d,
        .log_toggle = svc_log_toggle_handler_monit_d,
        .print_usage = print_usage_monit_d,
        .on_client = daemon_on_client_monit_d,
        .dispatcher = &g_dispatcher_monit_d,
        .event_driver = &g_event_driver_monit_d,
        .bsd = &g_bsd_monit_d,
        .bipc = &g_bipc_monit_d,
        .pool_max_events = 64,
        .pool_min = 2,
        .pool_max = 4,
        .pool_queue = 128,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, svc_activate, daemon_cupolas_init_pep),
    };
    return daemon_boot(argc, argv, &boot);
}
