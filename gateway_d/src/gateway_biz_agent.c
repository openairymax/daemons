// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_agent.c
 * @brief Gateway agent.run orchestration: dual-think injection +
 *        orchestration branches + session cancellation.
 *
 * The main chat path (no agent orchestration) first goes through think_d
 * dual-think (GCCP goal confirmation + GRAD plan critique) producing a
 * converged DAG plan, injects it into the LLM request context and hands it
 * to the tool loop; when params.agent is explicitly provided, it uses
 * agent_d orchestration (spawn + invoke). Supports agent.cancel manual
 * abort (session registry + tool-loop between-round checks).
 *
 * Split from gateway_business_handler.c (single responsibility: agent.run
 * orchestration).
 */

#include "gateway_biz_internal.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

#include "logging.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Generate a unique session ID (time + incrementing counter + random
 *        bits, avoiding time(NULL) pseudo-sessions)
 */
static void gw_gen_session_id(char *out, size_t out_size)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t rand_bits = 0;
    {
        uint64_t *p = (uint64_t *)&now;
        rand_bits = ((*p) ^ (s << 32)) * 6364136223846793005ULL;
    }
    snprintf(out, out_size, "sess_%016llx_%04llx", (unsigned long long)(now ^ rand_bits),
             (unsigned long long)(s & 0xFFFF));
}

/* Record one hall event for an agent.run session (best effort; a failed
 * event write never fails the run). `content` must be a JSON object. */
static void gw_agent_record_event(const char *session_id, const char *category, cJSON *content)
{
    if (!session_id || !session_id[0] || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    (void)gw_hall_store_event(session_id, category, NULL, content_str);
    AIRY_FREE(content_str);
}

/**
 * @brief Register an in-flight request (cancelled=0)
 */
static gw_active_request_t *gw_active_register(gateway_business_ctx_t *ctx, const char *session_id)
{
    gw_active_request_t *entry = (gw_active_request_t *)AIRY_CALLOC(1, sizeof(gw_active_request_t));
    if (!entry)
        return NULL;
    AIRY_STRNCPY_TERM(entry->session_id, session_id, sizeof(entry->session_id));
    atomic_store_explicit(&entry->cancelled, 0, memory_order_relaxed);
    airy_mtx_lock(&ctx->active_lock);
    entry->next = ctx->active_requests;
    ctx->active_requests = entry;
    airy_mtx_unlock(&ctx->active_lock);
    AIRY_LOG_INFO("gateway: agent.run registered (session=%s)", entry->session_id);
    return entry;
}

/**
 * @brief Unregister an in-flight request
 */
static void gw_active_unregister(gateway_business_ctx_t *ctx, gw_active_request_t *entry)
{
    if (!entry)
        return;
    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t **pp = (gw_active_request_t **)&ctx->active_requests;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }
    airy_mtx_unlock(&ctx->active_lock);
    AIRY_LOG_INFO("gateway: agent.run unregistered (session=%s)", entry->session_id);
    AIRY_FREE(entry);
}

/**
 * @brief Whether a cancellation has been requested
 */
bool gw_active_is_cancelled(gw_active_request_t *entry)
{
    return entry && atomic_load_explicit(&entry->cancelled, memory_order_relaxed) != 0;
}

/**
 * @brief Agent orchestration path: spawn + invoke (params.agent -> agent_d)
 * Calls agent_d's spawn(agent_spec) -> gets agent_id -> invoke(input=prompt)
 * -> returns output. Agent lifecycle is managed by agent_d (idle agents are
 * reaped automatically, see agent_service_reap_idle); the gateway holds no
 * agent state.
 *
 * @param ctx        Gateway context (contains agent_sock_path)
 * @param agent_spec params.agent (JSON object with role/language fields)
 * @param prompt     User input (used as invoke input)
 * @param out_text   Final output (AIRY_MALLOC, caller AIRY_FREE)
 * @param out_err    Failure reason (AIRY_MALLOC, caller AIRY_FREE; NULL on success)
 * @return 0 on success, non-zero on failure
 */
static int gw_agent_run_orchestrate(const gateway_business_ctx_t *ctx, const cJSON *agent_spec,
                                    const char *prompt, char **out_text, char **out_err)
{
    *out_text = NULL;
    *out_err = NULL;
    if (!cJSON_IsObject(agent_spec)) {
        *out_err = AIRY_STRDUP("params.agent must be a JSON object (role/language/...)");
        return -1;
    }

    char *spec_str = cJSON_PrintUnformatted(agent_spec);
    if (!spec_str) {
        *out_err = AIRY_STRDUP("cannot serialize params.agent");
        return -1;
    }

    size_t spawn_n = strlen(spec_str) + 32;
    char *spawn_params = (char *)AIRY_MALLOC(spawn_n);
    if (!spawn_params) {
        AIRY_FREE(spec_str);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    snprintf(spawn_params, spawn_n, "{\"agent_spec\":%s}", spec_str);
    AIRY_FREE(spec_str);

    char *spawn_resp =
        gw_svc_call(ctx->agent_sock_path, "spawn", spawn_params, GW_AGENT_SPAWN_TIMEOUT_MS);
    AIRY_FREE(spawn_params);
    if (!spawn_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (spawn)");
        return -1;
    }

    char *agent_id = NULL;
    cJSON *sroot = cJSON_Parse(spawn_resp);
    if (sroot) {
        cJSON *err = cJSON_GetObjectItem(sroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(sroot, "result");
        cJSON *aid = result ? cJSON_GetObjectItem(result, "agent_id") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(aid) && aid->valuestring) {
            agent_id = AIRY_STRDUP(aid->valuestring);
        } else {
            *out_err = AIRY_STRDUP("agent.spawn returned no agent_id");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.spawn returned invalid response");
    }
    cJSON_Delete(sroot);
    AIRY_FREE(spawn_resp);
    if (!agent_id) {
        if (!*out_err)
            *out_err = AIRY_STRDUP("agent.spawn failed");
        return -1;
    }

    cJSON *invoke_params = cJSON_CreateObject();
    if (!invoke_params) {
        AIRY_FREE(agent_id);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    cJSON_AddStringToObject(invoke_params, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke_params, "input", prompt ? prompt : "");
    char *invoke_params_str = cJSON_PrintUnformatted(invoke_params);
    cJSON_Delete(invoke_params);
    AIRY_FREE(agent_id);
    if (!invoke_params_str) {
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }

    char *invoke_resp =
        gw_svc_call(ctx->agent_sock_path, "invoke", invoke_params_str, GW_AGENT_INVOKE_TIMEOUT_MS);
    AIRY_FREE(invoke_params_str);
    if (!invoke_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (invoke)");
        return -1;
    }

    int rc = -1;
    cJSON *iroot = cJSON_Parse(invoke_resp);
    if (iroot) {
        cJSON *err = cJSON_GetObjectItem(iroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(iroot, "result");
        cJSON *out = result ? cJSON_GetObjectItem(result, "output") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(out)) {
            *out_text = AIRY_STRDUP(out->valuestring);
            rc = 0;
        } else {
            *out_err = AIRY_STRDUP("agent.invoke returned no output");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.invoke returned invalid response");
    }
    cJSON_Delete(iroot);
    AIRY_FREE(invoke_resp);
    return rc;
}

/* Find the top-level "role:" key in YAML-like content and return the start of
 * its value (skipping the colon and leading whitespace); NULL if not found.
 * Supports only the simplest key-value form; complex YAML should use JSON. */
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

/*
 * Parse an agent spec from params.agent_file (fallback when params.agent is
 * not provided directly):
 *   1. JSON file (e.g. {"role":"coding","language":"python"}) -> the whole
 *      object is used as the spec;
 *   2. Simple YAML (role: xxx line) -> extract role into {"role": xxx};
 *   3. Other plain text -> truncate the leading part as role.
 * Returns a newly allocated cJSON object (caller cJSON_Delete); NULL when no
 * valid spec is found.
 *
 * Design trade-off: the orchestration branch (spawn+invoke) only needs role to
 * derive an agent, so only the role field is parsed here; full declarations
 * such as language/capabilities are future extensions, not over-designed now.
 */
static cJSON *gw_agent_spec_from_agent_file(const cJSON *params)
{
    cJSON *af = cJSON_GetObjectItem(params, "agent_file");
    if (!cJSON_IsString(af) || !af->valuestring || !*af->valuestring)
        return NULL;

    FILE *f = fopen(af->valuestring, "rb");
    if (!f) {
        AIRY_LOG_WARN("gateway: agent_file unreadable: %s", af->valuestring);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC(GW_AGENT_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, GW_AGENT_FILE_MAX, f);
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
           end < GW_AGENT_FILE_MAX && end - start < 127)
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
            AIRY_LOG_INFO("gateway: agent spec built from agent_file (role=%s, file=%s)", role,
                     af->valuestring);
        }
    } else {
        AIRY_LOG_WARN("gateway: agent_file contains no usable role: %s", af->valuestring);
    }
    AIRY_FREE(buf);
    return spec;
}

/**
 * @brief Persist the current Q&A round to mem_d after the conversation (mem.write)
 *
 * M6 fix: mem_d previously had no business caller (dangling service). After
 * each successful agent.run, the gateway writes the user prompt + assistant
 * reply as one memory record to mem_d (metadata carries session_id/role) so
 * mem.search can recall it later.
 *
 * Called directly over the internal socket only (bypasses the
 * AIRY_GATEWAY_MEM_PUBLIC gate — that is the access switch for external mem.*
 * methods; internal persistence is unaffected). Failure only warns and never
 * blocks the response.
 */
static void gw_persist_conversation(const gateway_business_ctx_t *ctx, const char *session_id,
                                    const char *user_prompt, const char *assistant_text)
{
    if (!ctx || !ctx->mem_sock_path[0] || !user_prompt)
        return;

    size_t data_len = strlen(user_prompt) + strlen(assistant_text) + 24;
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

    char *resp = gw_svc_call(ctx->mem_sock_path, "write", params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        AIRY_LOG_WARN("gateway: mem.write failed (mem_d unreachable, session=%s)",
                 session_id ? session_id : "?");
        return;
    }
    AIRY_FREE(resp);
    AIRY_LOG_INFO("gateway: conversation persisted to mem_d (session=%s)",
             session_id ? session_id : "?");
}

char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *prompt = NULL;
    if (params) {
        cJSON *p = cJSON_GetObjectItem(params, "prompt");
        if (cJSON_IsString(p)) {
            prompt = p->valuestring;
        } else {
            cJSON *messages = cJSON_GetObjectItem(params, "messages");
            cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0) ?
                            cJSON_GetArrayItem(messages, 0) :
                            NULL;
            cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
            if (cJSON_IsString(c))
                prompt = c->valuestring;
        }
    }
    if (!prompt || !*prompt) {
        return jsonrpc_error(-32602, "Invalid params: missing prompt", id);
    }

    const char *model = ctx->default_model;
    if (params) {
        cJSON *m = cJSON_GetObjectItem(params, "model");
        if (cJSON_IsString(m) && m->valuestring && *m->valuestring)
            model = m->valuestring;
    }

    /* Full conversation history (OpenAI messages array, optional): when
     * non-empty it seeds the tool loop (M1/M2 fix — real multi-turn context
     * across requests); empty history degrades to a single prompt. */
    cJSON *history = params ? cJSON_GetObjectItem(params, "messages") : NULL;
    if (history && (!cJSON_IsArray(history) || cJSON_GetArraySize(history) == 0)) {
        history = NULL;
    }

    /* Branch: params.agent present -> agent_d orchestration (spawn+invoke);
     * otherwise keep the llm_d direct tool loop (backward compatible, D4). */
    cJSON *tool_trace = NULL;
    char *final_text = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;

    /* Session ID: the client may pre-assign one (agent.cancel needs to know
     * session_id before the request); otherwise the gateway generates a unique
     * ID (time + counter + random bits, not a time(NULL) pseudo-session). */
    char session_id[GW_SESSION_ID_LEN];
    cJSON *sid_param = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (cJSON_IsString(sid_param) && sid_param->valuestring && *sid_param->valuestring &&
        strlen(sid_param->valuestring) < GW_SESSION_ID_LEN &&
        strncmp(sid_param->valuestring, "sess_", 5) == 0) {
        AIRY_STRNCPY_TERM(session_id, sid_param->valuestring, sizeof(session_id));
    } else {
        gw_gen_session_id(session_id, sizeof(session_id));
    }

    gw_active_request_t *active = gw_active_register(ctx, session_id);

    /* Record the run start into the hall event flow (SSoT write side). */
    {
        cJSON *evt = cJSON_CreateObject();
        if (evt) {
            cJSON_AddStringToObject(evt, "event", "run_start");
            char pbuf[520];
            AIRY_STRNCPY_TERM(pbuf, prompt, sizeof(pbuf));
            cJSON_AddStringToObject(evt, "prompt", pbuf);
            gw_agent_record_event(session_id, "chain", evt);
            cJSON_Delete(evt);
        }
    }

    cJSON *agent_spec = params ? cJSON_GetObjectItem(params, "agent") : NULL;
    /* When params.agent is not provided, fall back to parsing
     * params.agent_file to build the spec: keeps old clients that only pass
     * agent_file working, so the orchestration branch still triggers.
     * agent_spec_owned must be freed at every exit of this function (unlike
     * agent_spec which points into params). */
    cJSON *agent_spec_owned = NULL;
    if (!agent_spec && params) {
        agent_spec_owned = gw_agent_spec_from_agent_file(params);
        agent_spec = agent_spec_owned;
    }
    AIRY_LOG_INFO("gateway: agent.run start (session=%s, model=%s, orchestrate=%d)", session_id,
             model ? model : "(default)", agent_spec ? 1 : 0);
    int run_rc = -1;

    cJSON *think_result = NULL;
    if (agent_spec) {
        char *err_msg = NULL;
        run_rc = gw_agent_run_orchestrate(ctx, agent_spec, prompt, &final_text, &err_msg);
        if (run_rc != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Agent orchestration failed: %s",
                     err_msg ? err_msg : "unknown error");
            AIRY_FREE(err_msg);
            gw_active_unregister(ctx, active);
            if (agent_spec_owned)
                cJSON_Delete(agent_spec_owned);
            return jsonrpc_error(-32603, msg, id);
        }
        /* On the orchestration path the tool trace is produced internally by
         * the runner (ecosystem/agents), invisible to the gateway; set
         * tool_trace to an empty array to keep the field contract. */
        tool_trace = cJSON_CreateArray();
    } else {
        /* The main dialog path uses dual thinking (D4 fix, 2026-08-07):
         * without agent orchestration, think_d runs GCCP (fact-lock goal
         * confirmation) + GRAD (logic-lock plan quadruple-check/final ruling)
         * first, producing a converged DAG plan and thinking events; the plan
         * is then injected into the LLM request context (system message) so
         * the LLM answers/executes according to the plan. If think_d is
         * unreachable/timed out, degrade to the original direct call (dialog
         * availability is unaffected). */
        if (gw_think_process(ctx, prompt, &think_result) == 0 && think_result) {
            cJSON *plan = cJSON_GetObjectItem(think_result, "plan");
            if (plan) {
                char *plan_str = cJSON_PrintUnformatted(plan);
                if (plan_str) {
                    /* Record the converged plan into the hall event flow. */
                    {
                        cJSON *pevt = cJSON_CreateObject();
                        if (pevt) {
                            cJSON_AddStringToObject(pevt, "event", "plan");
                            char pbuf[1024];
                            AIRY_STRNCPY_TERM(pbuf, plan_str, sizeof(pbuf));
                            cJSON_AddStringToObject(pevt, "plan", pbuf);
                            gw_agent_record_event(session_id, "chain", pevt);
                            cJSON_Delete(pevt);
                        }
                    }
                    /* Build a system message carrying the dual-thinking plan
                     * and insert it at the head of messages: the LLM structures
                     * its answer around the GCCP+GRAD converged DAG plan,
                     * avoiding made-up steps. */
                    cJSON *messages_with_plan = NULL;
                    if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {
                        messages_with_plan = cJSON_Duplicate(history, 1);
                    } else {
                        messages_with_plan = cJSON_CreateArray();
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
                    cJSON_AddItemToArray(messages_with_plan, sys);

                    cJSON *usr = cJSON_CreateObject();
                    cJSON_AddStringToObject(usr, "role", "user");
                    cJSON_AddStringToObject(usr, "content", prompt);
                    cJSON_AddItemToArray(messages_with_plan, usr);
                    AIRY_FREE(plan_str);

                    run_rc = gw_run_tool_loop(ctx, model, prompt, messages_with_plan, active,
                                              &tool_trace, &final_text, &total_tokens, &total_cost);
                    cJSON_Delete(messages_with_plan);
                } else {
                    run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace,
                                              &final_text, &total_tokens, &total_cost);
                }
            } else {
                run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace,
                                          &final_text, &total_tokens, &total_cost);
            }
        } else {

            run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace, &final_text,
                                      &total_tokens, &total_cost);
        }
    }
    /* Record the tool-call summary and the run result into the hall event
     * flow (SSoT write side). */
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
            gw_agent_record_event(session_id, "chain", tevt);
            cJSON_Delete(tevt);
        }
    }
    {
        cJSON *revt = cJSON_CreateObject();
        if (revt) {
            cJSON_AddStringToObject(revt, "event", "run_result");
            cJSON_AddNumberToObject(revt, "rc", run_rc);
            cJSON_AddNumberToObject(revt, "tokens", (double)total_tokens);
            cJSON_AddNumberToObject(revt, "cost", total_cost);
            char tbuf[520];
            AIRY_STRNCPY_TERM(tbuf, final_text ? final_text : "", sizeof(tbuf));
            cJSON_AddStringToObject(revt, "text", tbuf);
            gw_agent_record_event(session_id, "result", revt);
            cJSON_Delete(revt);
        }
    }

    gw_active_unregister(ctx, active);
    AIRY_LOG_INFO("gateway: agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)", session_id,
             run_rc, (unsigned long long)total_tokens, total_cost);

    /* Persist the conversation to mem_d automatically at the end (M6 fix:
     * mem_d is no longer a dangling service). Only written on success (rc==0);
     * user cancellation/failure produces no partial memory. */
    if (run_rc == 0) {
        gw_persist_conversation(ctx, session_id, prompt, final_text ? final_text : "");
    }

    if (run_rc == 1) {

        cJSON *err_out = cJSON_CreateObject();
        cJSON_AddStringToObject(err_out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(err_out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(err_out, "id");
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32800);
        cJSON_AddStringToObject(err, "message", "Request cancelled by user");
        cJSON_AddStringToObject(err, "data", session_id);
        cJSON_AddItemToObject(err_out, "error", err);
        char *err_str = cJSON_PrintUnformatted(err_out);
        cJSON_Delete(err_out);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (think_result)
            cJSON_Delete(think_result);
        if (final_text)
            AIRY_FREE(final_text);
        if (agent_spec_owned)
            cJSON_Delete(agent_spec_owned);
        return err_str;
    }
    if (run_rc != 0) {

        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (think_result)
            cJSON_Delete(think_result);
        if (final_text)
            AIRY_FREE(final_text);
        if (agent_spec_owned)
            cJSON_Delete(agent_spec_owned);
        return jsonrpc_error(-32603, "agent.run failed: tool loop exhausted or LLM service error",
                             id);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "session_id", session_id);
    cJSON_AddStringToObject(result, "response", final_text ? final_text : "");
    cJSON_AddNumberToObject(result, "tokens_used", (double)total_tokens);
    cJSON_AddNumberToObject(result, "cost_usd", total_cost);
    if (tool_trace) {
        cJSON_AddItemToObject(result, "tool_trace", tool_trace);
    } else {
        cJSON_AddItemToObject(result, "tool_trace", cJSON_CreateArray());
    }
    /* Attach the dual-thinking result (GCCP+GRAD DAG plan + thinking events +
     * stats). NULL when think_d was unreachable; the field is omitted for
     * backward compatibility with old clients. */
    if (think_result) {
        cJSON_AddItemToObject(result, "thinking", think_result);
        think_result = NULL;
    }
    cJSON_AddItemToObject(out, "result", result);

    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (final_text)
        AIRY_FREE(final_text);
    if (agent_spec_owned)
        cJSON_Delete(agent_spec_owned);
    return out_str;
}

/**
 * @brief agent.cancel: manually abort an in-flight agent.run request
 *
 * params.session_id -> look up the entry in the in-flight registry and set
 * cancelled. The tool loop checks this flag between rounds and stops, then
 * returns a -32800 error to the original request.
 *
 * @return JSON-RPC response (result.status="cancelling" on success; error if not found)
 */
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *sid = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring) {
        return jsonrpc_error(-32602, "Invalid params: missing session_id", id);
    }

    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t *entry = ctx->active_requests;
    while (entry) {
        if (strcmp(entry->session_id, sid->valuestring) == 0)
            break;
        entry = entry->next;
    }
    if (entry) {
        atomic_store_explicit(&entry->cancelled, 1, memory_order_relaxed);
        AIRY_LOG_INFO("gateway: agent.cancel set (session=%s)", sid->valuestring);
    }
    airy_mtx_unlock(&ctx->active_lock);

    if (!entry) {
        AIRY_LOG_DEBUG("gateway: agent.cancel miss (session=%s, 请求已完成或不存在)", sid->valuestring);
        return jsonrpc_error(-32004, "No active request with given session_id", id);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "cancelling");
    cJSON_AddStringToObject(result, "session_id", sid->valuestring);
    cJSON_AddItemToObject(out, "result", result);
    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return out_str;
}
