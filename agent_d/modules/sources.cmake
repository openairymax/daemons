# SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#
# @generated DO NOT EDIT
# 由 daemon_gen.py v1.15.0 自 .manifest 生成（L3 SSoT），禁手改。
# 手写域: src/svc.c 与 modules 业务源；本文件只做真装配。

set(AGENT_D_SOURCES
    src/main.c
    src/svc.c
    src/rpc/agent_d_rpc.c
    src/rpc/agent_run_rpc.c
    src/monitor/agent_d_monitor.c
    src/run/agent_run_engine.c
    src/run/agent_run_ledger.c
    src/run/agent_run_loop.c
    src/run/agent_run_stream.c
)
