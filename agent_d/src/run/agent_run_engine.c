// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_engine.c
 * @brief Agent run 进程内引擎（M1-1a 引擎下沉）。
 *
 * agent.run 由 gateway 迁入 agent_d：会话注册表与编排（spawn+invoke
 * 进程内）在本文件完成；一次 run 的阶段编排（含 think_d 双思考接入、
 * mem 会话持久化、hall 事件记录）见 agent_run_exec.c；工具循环（ReAct）
 * 见 agent_run_loop.c。
 *
 * 依赖经 daemon_rpc_client 直连各 daemon 服务（与 agent_d 既有
 * sched_d register_agent 调用同一路径）：
 *   - think_d.process   双思考（agent_run_exec.c）
 *   - mem_d.write       会话持久化（agent_run_exec.c）
 * 工具循环域（agent_run_loop.c）直连 llm_d/tool_d。
 */

#include "agent_run_engine.h"
#include "agent_run_internal.h"

#include "agent_d_internal.h"
#include "service/agent_service.h"
#include "airy_memory.h"
#include "airy_run_stream.h"
#include "atomic_compat.h"
#include "error.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

/* ---- run_stream 事件信封推送（M1-1d 协议先行） ---- */

/**
 * @brief 组装 §2.4 v1 事件信封 JSON 并交给 sink->emit。
 * data 所有权由本函数接管（作为 env 子项释放）；env 所有权亦由本函数持有，
 * sink 仅在调用期内借用（不得留存指针）。emit 不得阻塞。
 */
void agent_run_emit_event(const agent_run_event_sink_t *sink, uint64_t *seq,
                          const char *run_id, const char *session_id, const char *type,
                          cJSON *data)
{
    cJSON *env = NULL;
    if (sink && sink->emit && type && seq) {
        env = cJSON_CreateObject();
        if (env) {
            cJSON_AddNumberToObject(env, AIRY_RS_K_V, AIRY_RS_VERSION);
            cJSON_AddStringToObject(env, AIRY_RS_K_TYPE, type);
            cJSON_AddNumberToObject(env, AIRY_RS_K_ID, (double)(*seq)++);
            if (run_id && run_id[0])
                cJSON_AddStringToObject(env, AIRY_RS_K_RUN_ID, run_id);
            if (session_id && session_id[0])
                cJSON_AddStringToObject(env, AIRY_RS_K_SESSION, session_id);
            cJSON_AddNumberToObject(env, AIRY_RS_K_TS, (double)airy_time_ms());
            cJSON_AddNumberToObject(env, AIRY_RS_K_EPOCH, 0);
            if (data)
                cJSON_AddItemToObject(env, AIRY_RS_K_DATA, data);
        }
    }
    if (!env) {
        if (data)
            cJSON_Delete(data);
        return;
    }
    sink->emit(type, env, sink->ud);
    cJSON_Delete(env);
}

/* 会话注册表（agent.cancel 按 session_id 置位；并发客户端模型需加锁） */
static agent_run_session_t *g_run_sessions = NULL;
static airy_mtx_t g_run_lock;

static void run_lock_init(void)
{
    static int initialized = 0;
    if (!initialized) {
        airy_mtx_init(&g_run_lock);
        initialized = 1;
    }
}

agent_run_session_t *agent_run_register(const char *session_id)
{
    if (!session_id || !session_id[0])
        return NULL;
    run_lock_init();
    agent_run_session_t *s = (agent_run_session_t *)AIRY_CALLOC(1, sizeof(*s));
    if (!s)
        return NULL;
    AIRY_STRNCPY_TERM(s->session_id, session_id, sizeof(s->session_id));
    s->cancelled = 0;
    airy_mtx_lock(&g_run_lock);
    s->next = g_run_sessions;
    g_run_sessions = s;
    airy_mtx_unlock(&g_run_lock);
    return s;
}

void agent_run_unregister(agent_run_session_t *s)
{
    if (!s)
        return;
    run_lock_init();
    airy_mtx_lock(&g_run_lock);
    agent_run_session_t **pp = &g_run_sessions;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
        pp = &(*pp)->next;
    }
    airy_mtx_unlock(&g_run_lock);
    AIRY_FREE(s);
}

bool agent_run_is_cancelled(const agent_run_session_t *s)
{
    return s && s->cancelled != 0;
}

int agent_run_cancel_by_session(const char *session_id)
{
    if (!session_id || !session_id[0])
        return AIRY_ERR_NOT_FOUND;
    run_lock_init();
    airy_mtx_lock(&g_run_lock);
    int found = 0;
    for (agent_run_session_t *s = g_run_sessions; s; s = s->next) {
        if (strcmp(s->session_id, session_id) == 0) {
            s->cancelled = 1;
            found = 1;
            break;
        }
    }
    airy_mtx_unlock(&g_run_lock);
    return found ? AIRY_SUCCESS : AIRY_ERR_NOT_FOUND;
}

/* ID 生成：时间戳与单调序号混洗后取 16+4 位十六进制，前缀区分 sess/run。 */
static void run_gen_id(char *out, size_t out_size, const char *pfx)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t mix = (now ^ (s << 17)) * 6364136223846793005ULL;
    snprintf(out, out_size, "%s_%016llx_%04llx", pfx, (unsigned long long)(now ^ mix),
             (unsigned long long)(s & 0xFFFF));
}

void agent_run_gen_session_id(char *out, size_t out_size)
{
    run_gen_id(out, out_size, "sess");
}

void agent_run_gen_run_id(char *out, size_t out_size)
{
    run_gen_id(out, out_size, "run");
}

/* 编排失败原因：把子服务返回码翻译成可判读文本（阶段 + 契约错误符号）。
 * 该文本经 engine → response / error 事件 → RPC 逐层透出，是用户唯一的
 * 判读入口；禁止退化为通用话术（S-2 收敛）。 */
static char *agent_run_orch_reason(const char *stage, int rc)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "agent.%s failed: %s (%d)", stage,
             airy_err_code_name((airy_err_t)rc), rc);
    return AIRY_STRDUP(buf);
}

/* 编排分支：进程内 spawn+invoke（agent.run 单入口，无 RPC 环）。 */
int agent_run_orchestrate(const cJSON *agent_spec, const char *prompt, char **out_text,
                          char **out_err)
{
    *out_text = NULL;
    *out_err = NULL;
    if (!agent_spec || !cJSON_IsObject(agent_spec)) {
        *out_err = AIRY_STRDUP("params.agent must be a JSON object (role/language/...)");
        return -1;
    }

    char *spec_str = cJSON_PrintUnformatted(agent_spec);
    if (!spec_str) {
        *out_err = AIRY_STRDUP("cannot serialize params.agent");
        return -1;
    }

    char *agent_id = NULL;
    int rc = agent_service_spawn(g_service, spec_str, &agent_id);
    if (rc != AIRY_SUCCESS || !agent_id) {
        SVC_LOG_WARN("agent.run: spawn failed (rc=%d, %s)", rc,
                     airy_err_code_name((airy_err_t)rc));
        *out_err = agent_run_orch_reason("spawn", rc);
        AIRY_FREE(agent_id);
        AIRY_FREE(spec_str);
        return -1;
    }

    int ret = agent_service_invoke(g_service, agent_id, prompt, strlen(prompt), NULL, 0, NULL,
                                   out_text);
    AIRY_FREE(agent_id);
    AIRY_FREE(spec_str);
    if (ret != AIRY_SUCCESS || !*out_text) {
        SVC_LOG_WARN("agent.run: invoke failed (rc=%d, %s)", ret,
                     airy_err_code_name((airy_err_t)ret));
        *out_err = agent_run_orch_reason("invoke", ret);
        return -1;
    }
    return 0;
}
