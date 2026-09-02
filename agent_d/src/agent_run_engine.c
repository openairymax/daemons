// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_engine.c
 * @brief Agent run 进程内引擎（M1-1a 引擎下沉）。
 *
 * agent.run 由 gateway 迁入 agent_d：会话注册表、GCCP 双思考接入、
 * 编排（spawn+invoke 进程内）、mem 会话持久化与 hall 事件记录全部
 * 在本引擎内完成；工具循环（ReAct）见 agent_run_loop.c。
 *
 * 依赖经 daemon_rpc_client 直连各 daemon 服务（与 agent_d 既有
 * sched_d register_agent 调用同一路径）：
 *   - think_d.process   双思考
 *   - mem_d.write       会话持久化
 * 工具循环域（agent_run_loop.c）直连 llm_d/tool_d。
 */

#include "agent_run_engine.h"
#include "agent_run_internal.h"

#include "agent_d_internal.h"
#include "agent_service.h"
#include "airy_memory.h"
#include "airy_run_stream.h"
#include "atomic_compat.h"
#include "daemon_rpc_client.h"
#include "hall_writer.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_RUN_MODEL_DEFAULT "deepseek-v4-flash"
#define AGENT_RUN_LLM_MAX_RESP 1048576
#define AGENT_RUN_SOCK_BUF AIRY_PATH_MAX

/* ---- run_stream 事件信封推送（M1-1d 协议先行） ---- */

/**
 * @brief 组装 §2.4 v1 事件信封 JSON 并交给 sink->emit。
 * data 所有权转移给本函数（内部 cJSON_Print 后 Delete），emit 不得阻塞。
 */
void agent_run_emit_event(const agent_run_event_sink_t *sink, uint64_t *seq,
                          const char *run_id, const char *session_id, const char *type,
                          cJSON *data)
{
    if (!sink || !sink->emit || !type)
        return;
    cJSON *env = cJSON_CreateObject();
    if (!env) {
        if (data)
            cJSON_Delete(data);
        return;
    }
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
    sink->emit(type, env, sink->ud);
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

void agent_run_gen_session_id(char *out, size_t out_size)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t rand_bits = ((uint64_t *)&now)[0] ^ (s << 32);
    rand_bits = rand_bits * 6364136223846793005ULL;
    snprintf(out, out_size, "sess_%016llx_%04llx", (unsigned long long)(now ^ rand_bits),
             (unsigned long long)(s & 0xFFFF));
}

void agent_run_gen_run_id(char *out, size_t out_size)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t rand_bits = ((uint64_t *)&now)[0] ^ (s << 17);
    rand_bits = rand_bits * 2862933555777941757ULL;
    snprintf(out, out_size, "run_%016llx_%04llx", (unsigned long long)(now ^ rand_bits),
             (unsigned long long)(s & 0xFFFF));
}

/* 记录一条 hall 事件（决策链写侧，best effort：写失败不阻塞主流程） */
void agent_run_record_event(const char *session_id, const char *category, cJSON *content)
{
    if (!session_id || !session_id[0] || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    (void)daemon_hall_write(session_id, category, NULL, content_str);
    AIRY_FREE(content_str);
}

/* GCCP 双思考：think_d.process（降级容忍：think_d 不可达时返回非零，
 * 调用方回退直接 LLM 工具循环，对话可用性不受影响）。 */
int agent_run_think_process(const char *session_id, const char *prompt,
                            const char *gccp_answers, cJSON **out_think)
{
    *out_think = NULL;
    if (!prompt || !prompt[0])
        return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "prompt", prompt);
    if (session_id && session_id[0])
        cJSON_AddStringToObject(params, "session_id", session_id);
    if (gccp_answers && gccp_answers[0])
        cJSON_AddStringToObject(params, "gccp_answers", gccp_answers);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("think.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, "process", params_str, &resp, AGENT_RUN_THINK_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        SVC_LOG_WARN("agent.run: think.process unreachable, degrading to direct LLM");
        return -1;
    }

    /* daemon_rpc_call 已提取 result 字段；think.process 的 result 是
     * 双思考结果的 JSON 字符串（与旧 gateway 解析一致）。 */
    cJSON *think = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!think || !cJSON_IsObject(think)) {
        if (think)
            cJSON_Delete(think);
        return -1;
    }
    *out_think = think;
    return 0;
}

/* 会话持久化：成功后把本轮问答写入 mem_d（best effort）。 */
void agent_run_persist(const char *session_id, const char *user_prompt, const char *assistant_text)
{
    if (!user_prompt)
        return;
    size_t data_len = strlen(user_prompt) + (assistant_text ? strlen(assistant_text) : 0) + 24;
    char *data = (char *)AIRY_MALLOC(data_len);
    if (!data)
        return;
    snprintf(data, data_len, "user: %s\nassistant: %s", user_prompt,
             assistant_text ? assistant_text : "");

    cJSON *wparams = cJSON_CreateObject();
    cJSON *metadata = cJSON_CreateObject();
    if (!wparams || !metadata) {
        cJSON_Delete(wparams);
        cJSON_Delete(metadata);
        AIRY_FREE(data);
        return;
    }
    cJSON_AddStringToObject(wparams, "data", data);
    cJSON_AddStringToObject(metadata, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(metadata, "role", "agentrt");
    cJSON_AddItemToObject(wparams, "metadata", metadata);
    char *params_str = cJSON_PrintUnformatted(wparams);
    cJSON_Delete(wparams);
    AIRY_FREE(data);
    if (!params_str)
        return;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("mem.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, "write", params_str, &resp, AGENT_RUN_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        SVC_LOG_WARN("agent.run: mem.write failed (session=%s)", session_id ? session_id : "?");
    } else {
        AIRY_FREE(resp);
    }
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
        *out_err = AIRY_STRDUP("agent.spawn failed");
        AIRY_FREE(spec_str);
        return -1;
    }

    int ret = agent_service_invoke(g_service, agent_id, prompt, strlen(prompt), NULL, NULL,
                                   out_text);
    AIRY_FREE(agent_id);
    AIRY_FREE(spec_str);
    if (ret != AIRY_SUCCESS || !*out_text) {
        *out_err = AIRY_STRDUP("agent.invoke failed");
        return -1;
    }
    return 0;
}

/* 顶层 "role:" 键解析（简单 YAML；复杂 YAML 应使用 JSON） */
static const char *yaml_role_value(const char *buf)
{
    const char *p = buf;
    while ((p = strstr(p, "role")) != NULL) {
        const char *q = p + 4;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == ':')
            return q + 1;
        p = p + 4;
    }
    return NULL;
}

/* 从 params.agent_file 解析 agent spec（JSON / 简单 YAML / 纯文本首行）。 */
cJSON *agent_run_spec_from_file(const cJSON *params)
{
    cJSON *af = cJSON_GetObjectItem(params, "agent_file");
    if (!cJSON_IsString(af) || !af->valuestring || !*af->valuestring)
        return NULL;

    FILE *f = fopen(af->valuestring, "rb");
    if (!f) {
        SVC_LOG_WARN("agent.run: agent_file unreadable: %s", af->valuestring);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC(AGENT_RUN_AGENT_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, AGENT_RUN_AGENT_FILE_MAX, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *parsed = cJSON_Parse(buf);
    if (parsed) {
        if (cJSON_IsObject(parsed)) {
            AIRY_FREE(buf);
            return parsed;
        }
        cJSON_Delete(parsed);
    }

    const char *src = yaml_role_value(buf);
    if (!src)
        src = buf;
    size_t start = 0;
    while (src[start] == ' ' || src[start] == '\t')
        start++;
    size_t end = start;
    while (src[end] && src[end] != '\n' && src[end] != '\r' && src[end] != ',' &&
           end < AGENT_RUN_AGENT_FILE_MAX && end - start < 127)
        end++;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '"' ||
                           src[end - 1] == '\''))
        end--;

    cJSON *spec = NULL;
    if (end > start) {
        spec = cJSON_CreateObject();
        if (spec) {
            char role[128];
            size_t rlen = end - start;
            AIRY_MEMCPY(role, src + start, rlen);
            role[rlen] = '\0';
            cJSON_AddStringToObject(spec, "role", role);
            SVC_LOG_INFO("agent.run: agent spec built from agent_file (role=%s, file=%s)", role,
                         af->valuestring);
        }
    } else {
        SVC_LOG_WARN("agent.run: agent_file contains no usable role: %s", af->valuestring);
    }
    AIRY_FREE(buf);
    return spec;
}

/* ---- agent.run 主入口 ---- */

int agent_run_execute(const char *prompt, const char *model, const cJSON *history,
                      const char *gccp_answers, const cJSON *agent_spec,
                      const char *agent_file, const char *session_id,
                      const agent_run_event_sink_t *sink, cJSON **out_result)
{
    *out_result = NULL;
    if (!prompt || !prompt[0])
        return -1;

    char sess[AGENT_RUN_SESSION_ID_LEN];
    if (session_id && session_id[0] && strlen(session_id) < sizeof(sess) &&
        strncmp(session_id, "sess_", 5) == 0) {
        AIRY_STRNCPY_TERM(sess, session_id, sizeof(sess));
    } else {
        agent_run_gen_session_id(sess, sizeof(sess));
    }

    agent_run_session_t *active = agent_run_register(sess);
    if (!active)
        return -1;

    /* 每次运行独立 run_id（§2.4.2 信封锚点；run_start 帧起每帧携带） */
    agent_run_gen_run_id(active->run_id, sizeof(active->run_id));

    uint64_t seq = 0;

    /* run_start 事件（决策链写侧 + run_stream 流式推送） */
    {
        cJSON *evt = cJSON_CreateObject();
        if (evt) {
            cJSON_AddStringToObject(evt, "event", "run_start");
            char pbuf[520];
            AIRY_STRNCPY_TERM(pbuf, prompt, sizeof(pbuf));
            cJSON_AddStringToObject(evt, "prompt", pbuf);
            agent_run_record_event(sess, "chain", evt);
            cJSON_Delete(evt);
        }
        cJSON *rs = cJSON_CreateObject();
        if (rs) {
            char pbuf[520];
            AIRY_STRNCPY_TERM(pbuf, prompt, sizeof(pbuf));
            cJSON_AddStringToObject(rs, AIRY_RS_K_PROMPT, pbuf);
            if (model && model[0])
                cJSON_AddStringToObject(rs, AIRY_RS_K_MODEL, model);
            agent_run_emit_event(sink, &seq, active->run_id, sess, AIRY_RS_TYPE_RUN_START, rs);
        }
    }

    const char *mname = model && model[0] ? model : AGENT_RUN_MODEL_DEFAULT;
    int gccp_interact_round = 0;
    cJSON *think_result = NULL;
    cJSON *tool_trace = NULL;
    char *final_text = NULL;
    char *reasoning_acc = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    int run_rc = -1;

    /* 编排分支：params.agent 存在（或经 agent_file 解析出 spec） */
    cJSON *spec_owned = NULL;
    if (!agent_spec && agent_file) {
        cJSON *fake = cJSON_CreateObject();
        if (fake) {
            cJSON_AddStringToObject(fake, "agent_file", agent_file);
            spec_owned = agent_run_spec_from_file(fake);
            cJSON_Delete(fake);
        }
    }
    if (!agent_spec && spec_owned)
        agent_spec = spec_owned;

    if (agent_spec) {
        char *err_msg = NULL;
        run_rc = agent_run_orchestrate(agent_spec, prompt, &final_text, &err_msg);
        AIRY_FREE(err_msg);
        tool_trace = cJSON_CreateArray();
        run_rc = (run_rc == 0 && final_text) ? 0 : -1;
    } else {
        /* 主对话路径：GCCP 双思考 → 工具循环（降级容忍） */
        if (agent_run_think_process(sess, prompt, gccp_answers, &think_result) == 0 &&
            think_result) {
            cJSON *gccp_need = cJSON_GetObjectItem(think_result, "gccp_need_interaction");
            if (cJSON_IsTrue(gccp_need)) {
                gccp_interact_round = 1;
                tool_trace = cJSON_CreateArray();
                run_rc = 0;
                SVC_LOG_INFO("agent.run: GCCP interaction round (session=%s)", sess);
            } else {
                cJSON *plan = cJSON_GetObjectItem(think_result, "plan");
                if (plan) {
                    char *plan_str = cJSON_PrintUnformatted(plan);
                    if (plan_str) {
                        cJSON *pevt = cJSON_CreateObject();
                        if (pevt) {
                            cJSON_AddStringToObject(pevt, "event", "plan");
                            char pbuf[1024];
                            AIRY_STRNCPY_TERM(pbuf, plan_str, sizeof(pbuf));
                            cJSON_AddStringToObject(pevt, "plan", pbuf);
                            agent_run_record_event(sess, "chain", pevt);
                            cJSON_Delete(pevt);
                        }
                        /* 双思考 DAG 计划注入 system 消息首部 */
                        cJSON *messages = NULL;
                        if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {
                            messages = cJSON_Duplicate(history, 1);
                        } else {
                            messages = cJSON_CreateArray();
                        }
                        cJSON *sys = cJSON_CreateObject();
                        char sys_content[8192];
                        int sn = snprintf(sys_content, sizeof(sys_content),
                                          "You are executing a task under the AgentRT "
                                          "dual-thinking system (GCCP goal confirmation + "
                                          "GRAD plan critique). A verified action plan has "
                                          "been produced. Follow this DAG plan strictly:\n%s",
                                          plan_str);
                        if (sn > 0 && sn < (int)sizeof(sys_content))
                            cJSON_AddStringToObject(sys, "content", sys_content);
                        else
                            cJSON_AddStringToObject(sys, "content",
                                                    "Execute the user request following "
                                                    "the verified action plan.");
                        cJSON_AddStringToObject(sys, "role", "system");
                        cJSON_AddItemToArray(messages, sys);
                        cJSON *usr = cJSON_CreateObject();
                        cJSON_AddStringToObject(usr, "role", "user");
                        cJSON_AddStringToObject(usr, "content", prompt);
                        cJSON_AddItemToArray(messages, usr);
                        AIRY_FREE(plan_str);
                        /* plan 事件（run_stream 流式推送） */
                        if (plan) {
                            char *pstr = cJSON_PrintUnformatted(plan);
                            if (pstr) {
                                cJSON *ps = cJSON_CreateObject();
                                if (ps) {
                                    char plbuf[2048];
                                    AIRY_STRNCPY_TERM(plbuf, pstr, sizeof(plbuf));
                                    cJSON_AddStringToObject(ps, AIRY_RS_K_PLAN, plbuf);
                                    agent_run_emit_event(sink, &seq, active->run_id, sess, AIRY_RS_TYPE_PLAN, ps);
                                }
                                AIRY_FREE(pstr);
                            }
                        }
                        run_rc = agent_run_tool_loop(prompt, messages, mname, active, sink,
                                                     &tool_trace, &final_text, &total_tokens,
                                                     &total_cost, &reasoning_acc);
                        cJSON_Delete(messages);
                    }
                }
            }
        }
        if (run_rc < 0 && !gccp_interact_round && !think_result) {
            run_rc = agent_run_tool_loop(prompt, history, mname, active, sink, &tool_trace,
                                         &final_text, &total_tokens, &total_cost, &reasoning_acc);
        } else if (run_rc < 0 && !gccp_interact_round && think_result) {
            /* think_d 可达但无 plan：仍走工具循环（保持既有降级语义） */
            run_rc = agent_run_tool_loop(prompt, history, mname, active, sink, &tool_trace,
                                         &final_text, &total_tokens, &total_cost, &reasoning_acc);
        }
    }

    /* 取消检查：用户取消（run_rc==1 来自工具循环）或此处标志位 */
    if (!agent_run_is_cancelled(active) && run_rc == 0) {
        /* 工具 trace 摘要与 run_result 事件（决策链写侧） */
        if (tool_trace && cJSON_IsArray(tool_trace) && cJSON_GetArraySize(tool_trace) > 0) {
            cJSON *tevt = cJSON_CreateObject();
            if (tevt) {
                cJSON_AddStringToObject(tevt, "event", "tools");
                cJSON *tarr = cJSON_CreateArray();
                if (tarr) {
                    int tn = cJSON_GetArraySize(tool_trace);
                    for (int i = 0; i < tn && i < 32; i++) {
                        cJSON *t = cJSON_GetArrayItem(tool_trace, i);
                        cJSON *titem = cJSON_CreateObject();
                        if (titem) {
                            const char *tool = NULL;
                            const char *args = NULL;
                            const char *result = NULL;
                            cJSON *tf = cJSON_GetObjectItem(t, "tool");
                            cJSON *af = cJSON_GetObjectItem(t, "arguments");
                            cJSON *rf = cJSON_GetObjectItem(t, "result");
                            if (cJSON_IsString(tf))
                                tool = tf->valuestring;
                            if (cJSON_IsString(af))
                                args = af->valuestring;
                            if (cJSON_IsString(rf))
                                result = rf->valuestring;
                            cJSON_AddStringToObject(titem, "tool", tool ? tool : "");
                            char abuf[160];
                            AIRY_STRNCPY_TERM(abuf, args ? args : "", sizeof(abuf));
                            cJSON_AddStringToObject(titem, "args", abuf);
                            char rbuf[160];
                            AIRY_STRNCPY_TERM(rbuf, result ? result : "", sizeof(rbuf));
                            cJSON_AddStringToObject(titem, "result", rbuf);
                            cJSON_AddItemToArray(tarr, titem);
                        }
                    }
                    cJSON_AddItemToObject(tevt, "tools", tarr);
                }
                agent_run_record_event(sess, "chain", tevt);
                cJSON_Delete(tevt);
            }
        }
        if (!gccp_interact_round) {
            cJSON *revt = cJSON_CreateObject();
            if (revt) {
                cJSON_AddStringToObject(revt, "event", "run_result");
                cJSON_AddNumberToObject(revt, "rc", run_rc);
                cJSON_AddNumberToObject(revt, "tokens", (double)total_tokens);
                cJSON_AddNumberToObject(revt, "cost", total_cost);
                char tbuf[520];
                AIRY_STRNCPY_TERM(tbuf, final_text ? final_text : "", sizeof(tbuf));
                cJSON_AddStringToObject(revt, "text", tbuf);
                agent_run_record_event(sess, "result", revt);
                cJSON_Delete(revt);
            }
        }
    }

    bool cancelled = agent_run_is_cancelled(active);
    agent_run_unregister(active);
    SVC_LOG_INFO("agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)", sess, run_rc,
                 (unsigned long long)total_tokens, total_cost);

    /* 组装 result：字段契约与旧 gateway 一致（CLI/TUI/SDK 兼容） */
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (think_result)
            cJSON_Delete(think_result);
        if (spec_owned)
            cJSON_Delete(spec_owned);
        AIRY_FREE(final_text);
        AIRY_FREE(reasoning_acc);
        return -1;
    }
    cJSON_AddStringToObject(result, "session_id", sess);
    cJSON_AddStringToObject(result, "response", final_text ? final_text : "");
    cJSON_AddNumberToObject(result, "tokens_used", (double)total_tokens);
    cJSON_AddNumberToObject(result, "cost_usd", total_cost);
    if (reasoning_acc && reasoning_acc[0])
        cJSON_AddStringToObject(result, "reasoning", reasoning_acc);
    if (tool_trace) {
        cJSON_AddItemToObject(result, "tool_trace", tool_trace);
    } else {
        cJSON_AddItemToObject(result, "tool_trace", cJSON_CreateArray());
    }
    if (think_result) {
        if (gccp_interact_round) {
            cJSON_AddBoolToObject(result, "interaction_required", 1);
            cJSON *qstr = cJSON_GetObjectItem(think_result, "gccp_questions");
            if (cJSON_IsString(qstr) && qstr->valuestring) {
                cJSON *qjson = cJSON_Parse(qstr->valuestring);
                if (qjson)
                    cJSON_AddItemToObject(result, "gccp_questions", qjson);
                else
                    cJSON_AddStringToObject(result, "gccp_questions", qstr->valuestring);
            }
        }
        cJSON_AddItemToObject(result, "thinking", think_result);
    }
    if (run_rc == 0 && !gccp_interact_round) {
        agent_run_persist(sess, prompt, final_text ? final_text : "");
    }

    /* run_stream 流式收尾：message（整条最终消息）→ run_end / error */
    if (sink && sink->emit) {
        if (run_rc == 0 && !gccp_interact_round && final_text) {
            cJSON *msg = cJSON_CreateObject();
            if (msg) {
                cJSON_AddStringToObject(msg, AIRY_RS_K_ROLE, "assistant");
                cJSON_AddStringToObject(msg, AIRY_RS_K_CONTENT, final_text);
                if (reasoning_acc && reasoning_acc[0])
                    cJSON_AddStringToObject(msg, AIRY_RS_K_REASONING, reasoning_acc);
                agent_run_emit_event(sink, &seq, active->run_id, sess, AIRY_RS_TYPE_MESSAGE, msg);
            }
        }
        cJSON *rend = cJSON_CreateObject();
        if (rend) {
            const char *status = "completed";
            if (cancelled)
                status = "cancelled";
            else if (run_rc != 0)
                status = "failed";
            cJSON_AddStringToObject(rend, AIRY_RS_K_STATUS, status);
            cJSON_AddNumberToObject(rend, AIRY_RS_K_DURATION, (double)airy_time_ms());
            cJSON_AddNumberToObject(rend, AIRY_RS_K_USE_TICKS, (double)total_tokens);
            agent_run_emit_event(sink, &seq, active->run_id, sess, AIRY_RS_TYPE_RUN_END, rend);
        }
        if (run_rc != 0 && !cancelled) {
            cJSON *err = cJSON_CreateObject();
            if (err) {
                cJSON_AddNumberToObject(err, AIRY_RS_K_CODE, -32603);
                cJSON_AddStringToObject(err, AIRY_RS_K_MSG,
                                        final_text && final_text[0] ?
                                            final_text :
                                            "agent.run failed: tool loop exhausted or LLM "
                                            "service error");
                cJSON_AddBoolToObject(err, AIRY_RS_K_RECOVER, 1);
                agent_run_emit_event(sink, &seq, active->run_id, sess, AIRY_RS_TYPE_ERROR, err);
            }
        }
    }

    /* 用户取消：走 -32800 错误语义由上层 RPC 适配（此处以 rc=1 透传） */
    *out_result = result;
    if (spec_owned)
        cJSON_Delete(spec_owned);
    AIRY_FREE(final_text);
    AIRY_FREE(reasoning_acc);
    return cancelled ? 1 : run_rc;
}
