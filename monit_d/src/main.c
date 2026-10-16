/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.15.0 (L3 SSoT)；改 .manifest 后重跑 --gen。 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_monit_d.h"

#include "daemon_main.h"
#include "daemon_security_dome.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(monit_d, monitor, MONIT_D_TCP_PORT)

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
};

DAEMON_MAIN(monit_d, MONIT_D, "monitor", "AIRY_MONIT_D_DEBUG",
            "monitor", "monitor,core", 12, 64, 2, 4, 128, 0,
            svc_activate, svc_attach, svc_teardown_noop,
            daemon_dome_init_pep, daemon_dome_cleanup)
