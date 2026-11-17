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

#include "airy_defaults.h"
#include "airy_memory.h"
#include "error.h"
#include "think_service.h"

#include "cognition.h"
#include "llm_svc_adapter.h"
#include "tool_svc_adapter.h"
#include "orchestrator.h"
#include "airy_orch_ops.h"
#include "svc_logger.h"
#include "svc_model_watch.h"

#include <cjson/cJSON.h>
#include <string.h>

/* airy_thread_t/airy_platform_thread_create：跨平台线程原语（POSIX =
 * pthread_t/pthread_create；WIN32 = HANDLE/CreateThread，platform_sync.c）。
 * 原始 pthread_t/pthread_create 在 MSVC 无实现（#118 实证 think_d LNK2001
 * pthread_create），wave-1（daemons/svc）同型收敛。 */
#include <platform.h>

/* think/orchestrate 超时缺省：SSoT 在 commons airy_defaults.h（与
 * think_d svc.c、airy_cli /orch 客户端共用），运行时被 model.yaml
 * [think] timeout_ms / env AIRY_THINK_TIMEOUT_MS 覆盖。 */
#define THINK_DEFAULT_TIMEOUT_MS AIRY_THINK_TIMEOUT_DEFAULT_MS
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
    airy_thread_t thread;
} think_orch_run_t;

struct think_service {
    airy_cognition_engine_t *engine;
    llm_svc_adapter_t *llm_adapter;
    /* C-12：GRAD 计划的执行通道（tool_d IPC）。engine 只持有借用指针，
     * 生命周期由 svc 在 engine 之后释放。 */
    tool_svc_adapter_t *tool_adapter;

    orchestrator_t *orch;
    think_orch_run_t runs[THINK_ORCH_MAX_RUNS];
    int next_run_id;
    airy_mtx_t orch_lock;

    char think2_slow_model[128];
    char think1_fast_model[128];
    char think1_prof_model[128];
    uint32_t process_timeout_ms;
    /* C-19：三角色槽位存在空值（回落 default_model），本轮结果须标记
     * grad_degraded，禁止把角色模型缺省当成正常配置静默放行。 */
    unsigned slot_deg;

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

    /* 0.1.19 §3.5 缺陷5：model.yaml 启动快照；C-19 起请求侧检出演进后
     * 热重载 slots 段（校验通过才应用，失败保留旧槽位） */
    svc_mw_t mw;
};

/* think_orch.c 导出至 think_service.c */
void think_orch_ops_inject(think_service_t *svc);

/* think_service.c 导出至 think_orch.c（全局 svc 指针供 ops 表使用） */
/* g_think_svc 由 think_orch.c 自身定义 */

#endif /* AIRY_THINK_ORCH_INTERNAL_H */
