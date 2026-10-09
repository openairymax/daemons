/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.12.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_cupolas_d.h"

#include "daemon_main.h"
#include "daemon_security_dome.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(cupolas_d, cupolas,
                      CUPOLAS_D_SOCKET_UNIX, CUPOLAS_D_SOCKET_WIN,
                      CUPOLAS_D_TCP_PORT, CUPOLAS_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(cupolas_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_CUPOLAS_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "cupolas_d",
        .cname = "cupolas",
        .env_debug = "AIRY_CUPOLAS_D_DEBUG",
        .sd_type = "cupolas",
        .tags = "cupolas,security",
        .method_total = 22,
        .running_lock = &g_running_lock_cupolas_d,
        .signal_handler = signal_handler_cupolas_d,
        .log_toggle = svc_log_toggle_handler_cupolas_d,
        .print_usage = print_usage_cupolas_d,
        .on_client = daemon_on_client_cupolas_d,
        .dispatcher = &g_dispatcher_cupolas_d,
        .event_driver = &g_event_driver_cupolas_d,
        .bsd = &g_bsd_cupolas_d,
        .pool_max_events = 64,
        .pool_min = 4,
        .pool_max = 8,
        .pool_queue = 256,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, daemon_svc_noop, daemon_dome_init, daemon_dome_cleanup),
    };
    return daemon_boot(argc, argv, &boot);
}
