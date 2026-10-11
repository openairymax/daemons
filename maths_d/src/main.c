/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.13.2 (L3 SSoT) 生成。
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
        DAEMON_BOOT_FILL(maths_d, "maths", "AIRY_MATHS_D_DEBUG",
                         "maths", "maths,core", 19, 64, 2, 4, 256, 0)
        DAEMON_BOOT_WIRE(SVC_OPS, SVC_METHODS,
                         svc_activate, svc_attach_noop,
                         svc_teardown, daemon_dome_init_pep,
                         daemon_dome_cleanup),
    };
    return daemon_boot(argc, argv, &boot);
}
