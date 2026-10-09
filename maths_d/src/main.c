/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.12.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_maths_d.h"

#include "daemon_main.h"
#include "daemon_security_dome.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(maths_d, maths,
                      MATHS_D_SOCKET_UNIX, MATHS_D_SOCKET_WIN,
                      MATHS_D_TCP_PORT, MATHS_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(maths_d)

static const daemon_method_entry_t SVC_METHODS[] = {
    SVC_MATHS_D_METHODS(DAEMON_METHOD_ENTRY)
};

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
};

int main(int argc, char **argv)
{
    daemon_boot_t boot = {
        .daemon = "maths_d",
        .cname = "maths",
        .env_debug = "AIRY_MATHS_D_DEBUG",
        .sd_type = "maths",
        .tags = "maths,core",
        .method_total = 19,
        .running_lock = &g_running_lock_maths_d,
        .signal_handler = signal_handler_maths_d,
        .log_toggle = svc_log_toggle_handler_maths_d,
        .print_usage = print_usage_maths_d,
        .on_client = daemon_on_client_maths_d,
        .dispatcher = &g_dispatcher_maths_d,
        .event_driver = &g_event_driver_maths_d,
        .bsd = &g_bsd_maths_d,
        .pool_max_events = 64,
        .pool_min = 2,
        .pool_max = 4,
        .pool_queue = 256,
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS, svc_activate, daemon_dome_init_pep, daemon_dome_cleanup),
    };
    return daemon_boot(argc, argv, &boot);
}
