/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.15.0 (L3 SSoT)；改 .manifest 后重跑 --gen。 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_tool_d.h"

#include "daemon_main.h"
#include "daemon_security_dome.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_llm_ops_bootstrap.h"
#include "daemon_tool_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(tool_d, tool, TOOL_D_TCP_PORT)

static const daemon_op_t SVC_OPS[] = {
    { daemon_ipc_ops_init, daemon_ipc_ops_cleanup },
    { daemon_llm_ops_init, daemon_llm_ops_cleanup },
    { daemon_tool_ops_init, daemon_tool_ops_cleanup },
};

DAEMON_MAIN(tool_d, TOOL_D, "tool", "AIRY_TOOL_D_DEBUG",
            "tool", "tool,core", 11, 64, 4, 8, 256, 1,
            svc_activate, svc_attach, svc_teardown_noop,
            daemon_dome_init_pep, daemon_dome_cleanup)
