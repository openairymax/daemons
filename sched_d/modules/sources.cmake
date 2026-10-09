# SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#
# @generated DO NOT EDIT
# 由 daemon_gen.py v1.12.0 自 .manifest 生成（L3 SSoT），禁手改。
# 手写域: src/svc.c 与 modules 业务源；本文件只做真装配。

set(SCHED_D_SOURCES
    src/main.c
    src/svc.c
    src/sched_rpc_handlers.c
    src/sched_dispatch.c
    src/roadmap_rpc.c
    src/sched_service_impl.c
    src/sched_service_agent.c
    src/sched_service_task.c
    src/sched_service_worker.c
    src/sched_dag_impl.c
    src/sched_dag_engine.c
    src/sched_dag_worker.c
    src/sched_dag_parse.c
)
