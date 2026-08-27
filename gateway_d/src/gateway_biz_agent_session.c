// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_agent_session.c
 * @brief Gateway agent.run 会话/编排域：会话 ID 生成、in-flight 会话注册表、
 *        agent_d 编排（spawn+invoke）、agent_file spec 解析与 mem_d 会话持久化。
 *
 * 2026-08-27 由 gateway_biz_agent.c 按单一职责拆分；主入口 handle_agent_run /
 * handle_agent_cancel 仍留在 gateway_biz_agent.c，共享符号经
 * gateway_biz_internal.h 声明。
 */

#include "gateway_biz_internal.h"

/* Gateway-side hall event recording (write side of the SSoT event flow) */
#include "gateway_hall_store.h"

#include "logging.h"
#include "platform.h"

#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Generate a unique session ID (time + incrementing counter + random
 *        bits, avoiding time(NULL) pseudo-sessions)
 */
void gw_gen_session_id(char *out, size_t out_size)
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
void gw_agent_record_event(const char *session_id, const char *category, cJSON *content)
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
gw_active_request_t *gw_active_register(gateway_business_ctx_t *ctx, const char *session_id)
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
void gw_active_unregister(gateway_business_ctx_t *ctx, gw_active_request_t *entry)
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
int gw_agent_run_orchestrate(const gateway_business_ctx_t *ctx, const cJSON *agent_spec,
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

    /* 架构约束 2026-08-25 "必须走 syscall": agent.spawn 经 SYS_SVC_CALL 派发 */
    char *spawn_resp = NULL;
    airy_err_t spawn_rc = airy_sys_svc_call("agent", "spawn", spawn_params,
                                            GW_AGENT_SPAWN_TIMEOUT_MS, &spawn_resp);
    AIRY_FREE(spawn_params);
    if (spawn_rc != AIRY_SUCCESS || !spawn_resp) {
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

    /* 架构约束 2026-08-25 "必须走 syscall": agent.invoke 经 SYS_SVC_CALL 派发 */
    char *invoke_resp = NULL;
    airy_err_t invoke_rc = airy_sys_svc_call("agent", "invoke", invoke_params_str,
                                             GW_AGENT_INVOKE_TIMEOUT_MS, &invoke_resp);
    AIRY_FREE(invoke_params_str);
    if (invoke_rc != AIRY_SUCCESS || !invoke_resp) {
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
cJSON *gw_agent_spec_from_agent_file(const cJSON *params)
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
void gw_persist_conversation(const gateway_business_ctx_t *ctx, const char *session_id,
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

    /* 架构约束 2026-08-25 "必须走 syscall": mem.write 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t mrc = airy_sys_svc_call("mem", "write", params_str, GW_TOOL_TIMEOUT_MS, &resp);
    AIRY_FREE(params_str);
    if (mrc != AIRY_SUCCESS || !resp) {
        AIRY_LOG_WARN("gateway: mem.write failed (mem_d unreachable, session=%s)",
                 session_id ? session_id : "?");
        return;
    }
    AIRY_FREE(resp);
    AIRY_LOG_INFO("gateway: conversation persisted to mem_d (session=%s)",
             session_id ? session_id : "?");
}
