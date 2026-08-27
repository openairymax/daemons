// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file think_orch_internal.h
 * @brief think_service.c ↔ think_orch.c 共享的内部类型与声明。
 *
 * 2026-08-27 域拆分：将编排器集成（orchestrator + ops 表）从
 * think_service.c 提取至 think_orch.c，共享结构体定义与内部函数
 * 声明统一于此。此头仅限 think_d/src 内部使用。
 */

#ifndef AIRY_THINK_ORCH_INTERNAL_H
#define AIRY_THINK_ORCH_INTERNAL_H

#include "airy_memory.h"
#include "error.h"
#include "think_service.h"

#include "cognition.h"
#include "llm_svc_adapter.h"
#include "orchestrator.h"
#include "airy_orch_ops.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <pthread.h>
#include <string.h>

#define THINK_DEFAULT_TIMEOUT_MS 120000u
#define THINK_DEFAULT_MAX_EVENTS 64u
#define THINK_MAX_EVENT_DATA_LEN 1024u
#define THINK_ORCH_MAX_RUNS 32u
#define THINK_ORCH_RUN_ID_LEN 48u

/* GCCP 交互状态会话隔离 */
#define THINK_GCCP_MAX_SESSIONS 64u
#define THINK_GCCP_SESSION_ID_LEN 96u
#define THINK_GCCP_TTL_MS (30u * 60u * 1000u)

typedef struct {
    char session_id[THINK_GCCP_SESSION_ID_LEN];
    char *questions;
    char *answers;
    uint64_t updated_ms;
} think_gccp_session_t;

typedef struct {
    int level;
    char module[64];
    char event[96];
    char data[THINK_MAX_EVENT_DATA_LEN];
} think_feedback_event_t;

/* Orchestrator 异步运行槽 */
typedef struct {
    int run_id;
    volatile int state; /* 1=running 2=completed 3=failed 4=cancelled */
    char *input;
    orch_result_t *results;
    size_t result_count;
    int exit_code;
    pthread_t thread;
} think_orch_run_t;

struct think_service {
    airy_cognition_engine_t *engine;
    llm_svc_adapter_t *llm_adapter;

    orchestrator_t *orch;
    think_orch_run_t runs[THINK_ORCH_MAX_RUNS];
    int next_run_id;
    airy_mtx_t orch_lock;

    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
    uint32_t process_timeout_ms;

    think_feedback_event_t *events;
    uint32_t event_capacity;
    uint32_t event_count;
    uint32_t event_head;

    uint32_t dual_invocations;
    uint32_t dual_corrections;
    airy_mtx_t lock;

    think_gccp_session_t gccp_sessions[THINK_GCCP_MAX_SESSIONS];
    char active_session[THINK_GCCP_SESSION_ID_LEN];
    uint64_t gccp_last_expire_ms;
};

/* think_orch.c 导出至 think_service.c */
void think_orch_ops_inject(think_service_t *svc);

/* think_service.c 导出至 think_orch.c（全局 svc 指针供 ops 表使用） */
/* g_think_svc 由 think_orch.c 自身定义 */

#endif /* AIRY_THINK_ORCH_INTERNAL_H */
