// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_invoke.c
 * @brief Agent service invocation domain: agent terminate/invoke (child
 *        stdin/stdout pipe protocol) and invoke-session
 *        register/unregister/cancel management.
 */

#include "io.h"
#include "platform_misc.h"

#include "agent_service_internal.h"

/* Resolve the agent entry under the service lock; NULL when absent. */
static agent_entry_internal_t *inv_lookup(agent_service_t *svc, const char *agent_id)
{
    agent_lock_svc(svc);
    ssize_t idx = hindex_get(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    agent_entry_internal_t *agent = &svc->agents[idx];
    airy_mtx_unlock(&svc->lock);
    return agent;
}

int agent_service_terminate(agent_service_t *svc, const char *agent_id)
{
    if (!svc || !svc->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_atomic_fetch_add(&svc->m_terminate_total, 1);

    agent_entry_internal_t *agent = inv_lookup(svc, agent_id);
    if (!agent)
        return AIRY_ERR_NOT_FOUND;

    airy_mtx_lock(&agent->entry_lock);
#if AIRY_PLATFORM_POSIX
    if (agent->child_pid > 0) {
        agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
    }
#endif

    agent->status = AGENT_STATUS_TERMINATED;
    airy_mtx_unlock(&agent->entry_lock);

    SVC_LOG_DEBUG("Agent terminate: agent_id=%s", agent_id);
    return AIRY_SUCCESS;
}

/* Effective read budget: honor a smaller caller deadline so this layer fails
 * first and frees the worker (0.1.16 budget inversion: inner 300s vs CLI
 * review outer 180s). */
static int inv_budget(int limit_s)
{
    int budget = agent_invoke_timeout_s();
    return (limit_s > 0 && limit_s < budget) ? limit_s : budget;
}

/* Every early rejection reports the same way: fixed JSON body plus a failure
 * counter tick. */
static int inv_reject(agent_service_t *svc, char **out_output, const char *json, int err)
{
    *out_output = AIRY_STRDUP(json);
    airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
    return err;
}

#if AIRY_PLATFORM_POSIX
/* Child exchange status codes; INV_FALLBACK keeps the entry lock held so the
 * caller can run the fallback path. */
enum { INV_OK = 0, INV_FALLBACK = 1, INV_NOBUF = 2, INV_IO = 3 };

/* Build the request line and push it to the child stdin. The newline is a
 * second write: the entry lock serializes writers, so the child never sees an
 * interleaved line. */
static int inv_send(agent_entry_internal_t *agent, const char *agent_id, const char *input,
                    const char *workspace_dir)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "agent_id", agent_id);
    cJSON_AddStringToObject(req, "input", input ? input : "");
    /* Decision E workspace isolation: forward the isolated workspace dir so
     * the child chdir()s into it before executing. Without this the agent
     * runs in the daemon's cwd and spends its tool rounds exploring the host
     * tree instead of acting on the task (P0-1). */
    if (workspace_dir && workspace_dir[0])
        cJSON_AddStringToObject(req, "workspace_dir", workspace_dir);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return INV_NOBUF;

    ssize_t wrc = airy_io_write_all(agent->stdin_fd, req_str, strlen(req_str));
    if (wrc >= 0)
        wrc = airy_io_write_all(agent->stdin_fd, "\n", 1);
    AIRY_FREE(req_str);
    return wrc < 0 ? INV_IO : INV_OK;
}

/* Mark a truncated child response so callers detect the suffix instead of
 * silently losing data. */
static void inv_mark_trunc(char *buf, int rrc, const char *agent_id)
{
    static const char SUFFIX[] = "\n...[agent response truncated]";
    if (rrc != 1)
        return;
    size_t slen = strlen(buf);
    size_t avail = AGENT_RESP_BUF_SIZE - slen - 1;
    if (avail >= sizeof(SUFFIX) - 1)
        AIRY_MEMCPY(buf + slen, SUFFIX, sizeof(SUFFIX));
    else if (avail > 1)
        AIRY_MEMCPY(buf + slen, SUFFIX, avail - 1);
    SVC_LOG_WARN("Agent invoke response truncated at %d bytes (agent_id=%s)",
                 (int)AGENT_RESP_BUF_SIZE, agent_id);
}

/* Extract the runner.py response object. A failure must NOT be flattened to
 * plain text with a SUCCESS return, otherwise upper layers absorb the error
 * string as a valid task result (P0-1 L2 poisoning). */
static void inv_take(cJSON *resp, const char *raw, char **out_output)
{
    cJSON *ok = cJSON_GetObjectItem(resp, "success");
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    *out_output = NULL;
    if (!(ok && cJSON_IsFalse(ok) && err && cJSON_IsString(err))) {
        cJSON *out = cJSON_GetObjectItem(resp, "output");
        *out_output = (out && cJSON_IsString(out)) ? AIRY_STRDUP(out->valuestring) : AIRY_STRDUP(raw);
        return;
    }
    cJSON *fobj = cJSON_CreateObject();
    if (fobj) {
        cJSON_AddBoolToObject(fobj, "success", 0);
        cJSON_AddStringToObject(fobj, "error", err->valuestring);
        *out_output = cJSON_PrintUnformatted(fobj);
    }
    cJSON_Delete(fobj);
    if (!*out_output)
        *out_output = AIRY_STRDUP("{\"success\":false,\"error\":\"agent failed\"}");
}

/* Child pipe protocol: one JSON request line in, one JSON response line out.
 * Returns INV_FALLBACK with the entry lock still held when the child is
 * unusable. */
static int inv_child(agent_service_t *svc, agent_entry_internal_t *agent, const char *agent_id,
                     const char *input, const char *workspace_dir, int read_timeout_s,
                     airy_cancel_token_t *cancel_token, char **out_output, uint64_t perf_t0)
{
    int src = inv_send(agent, agent_id, input, workspace_dir);
    if (src == INV_IO) {
        SVC_LOG_WARN("Agent invoke write failed, child unusable: agent_id=%s errno=%d", agent_id,
                     errno);
        agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
        return INV_FALLBACK;
    }
    if (src != INV_OK) {
        airy_mtx_unlock(&agent->entry_lock);
        *out_output = AIRY_STRDUP("{\"error\":\"failed to build request\"}");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    agent->last_active = (uint64_t)time(NULL);

    char *resp_buf = (char *)AIRY_MALLOC(AGENT_RESP_BUF_SIZE);
    if (!resp_buf) {
        airy_mtx_unlock(&agent->entry_lock);
        *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    int rrc = agent_read_line_timeout_ex(agent->stdout_fd, resp_buf, AGENT_RESP_BUF_SIZE,
                                         read_timeout_s, cancel_token);
    if (rrc == -2) {
        /* Cancel terminates the child gracefully and reports AbortedOutput,
         * clearly distinguished from the timeout fallback. */
        AIRY_FREE(resp_buf);
        SVC_LOG_WARN("Agent invoke canceled, terminating child: agent_id=%s", agent_id);
        agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
        airy_mtx_unlock(&agent->entry_lock);
        *out_output = AIRY_STRDUP("{\"success\":false,\"error\":\"aborted\",\"aborted\":true}");
        airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
        agent_perf_accumulate(&svc->m_invoke_us_total, &svc->m_invoke_us_max,
                              agent_perf_now_us() - perf_t0);
        return AIRY_ERR_CANCELED;
    }
    if (rrc < 0) {
        AIRY_FREE(resp_buf);
        SVC_LOG_WARN("Agent invoke read failed, child unusable: agent_id=%s", agent_id);
        agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
        return INV_FALLBACK;
    }

    agent->last_active = (uint64_t)time(NULL);
    inv_mark_trunc(resp_buf, rrc, agent_id);

    cJSON *resp = cJSON_Parse(resp_buf);
    if (resp) {
        inv_take(resp, resp_buf, out_output);
        cJSON_Delete(resp);
    } else {
        *out_output = AIRY_STRDUP(resp_buf);
    }
    AIRY_FREE(resp_buf);

    airy_mtx_unlock(&agent->entry_lock);

    airy_atomic_fetch_add(&svc->m_invoke_ok, 1);
    agent_perf_accumulate(&svc->m_invoke_us_total, &svc->m_invoke_us_max,
                          agent_perf_now_us() - perf_t0);
    SVC_LOG_DEBUG("Agent invoke via child: agent_id=%s", agent_id);
    return AIRY_SUCCESS;
}
#endif

int agent_service_invoke(agent_service_t *svc, const char *agent_id, const char *input, size_t len,
                         const char *workspace_dir, int timeout_s, airy_cancel_token_t *cancel_token,
                         char **out_output)
{
    if (!svc || !svc->initialized || !agent_id || !out_output)
        return AIRY_ERR_INVALID_PARAM;

    int read_timeout_s = inv_budget(timeout_s);
    *out_output = NULL;

    uint64_t perf_t0 = agent_perf_now_us();
    airy_atomic_fetch_add(&svc->m_invoke_total, 1);

    agent_entry_internal_t *agent = inv_lookup(svc, agent_id);
    if (!agent)
        return inv_reject(svc, out_output, "{\"error\":\"Agent not found\"}", AIRY_ERR_NOT_FOUND);

    /* Child communication runs under the fine-grained lock: calls to the same
     * Agent are serialized, calls to different Agents do not block each
     * other, supporting thousands of Agents running in parallel. */
    airy_mtx_lock(&agent->entry_lock);

    if (agent->status != AGENT_STATUS_RUNNING) {
        airy_mtx_unlock(&agent->entry_lock);
        return inv_reject(svc, out_output, "{\"error\":\"Agent not running\"}",
                          AIRY_ERR_STATE_ERROR);
    }

#if AIRY_PLATFORM_POSIX
    if (agent->child_pid > 0 && agent->stdin_fd >= 0) {
        int crc = inv_child(svc, agent, agent_id, input, workspace_dir, read_timeout_s,
                            cancel_token, out_output, perf_t0);
        if (crc != INV_FALLBACK)
            return crc;
    }
#endif

    /* P0-2: no child or communication failure -> return a clear error instead
     * of faking "invocation processed" success. Upper layers (taskflow/SDK)
     * use this to detect agent unavailability and avoid silently swallowing
     * faults. */
    (void)input;
    (void)len;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "agent_id", agent_id);
    cJSON_AddStringToObject(result, "error", "agent child process unavailable");
    *out_output = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    airy_mtx_unlock(&agent->entry_lock);

    airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
    agent_perf_accumulate(&svc->m_invoke_us_total, &svc->m_invoke_us_max,
                          agent_perf_now_us() - perf_t0);

    SVC_LOG_WARN("Agent invoke fallback (no child): agent_id=%s", agent_id);
    return AIRY_ERR_SVC_NOT_READY;
}

/* Session table lookup; the caller holds session_lock. */
static agent_invoke_session_t *sess_find(agent_service_t *svc, const char *request_id)
{
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (s->active && strcmp(s->request_id, request_id) == 0)
            return s;
    }
    return NULL;
}

int agent_service_invoke_begin(agent_service_t *svc, const char *request_id,
                               airy_cancel_token_t **out_token)
{
    if (!svc || !svc->initialized || !request_id || !out_token || request_id[0] == '\0')
        return AIRY_ERR_INVALID_PARAM;
    *out_token = NULL;

    /* The session owns the token: begin allocates/initializes, end unregisters
     * and destroys. cancel only sets the token flag (idempotent) without
     * destroying it, avoiding a race with the invoke thread. */
    airy_cancel_token_t *token = (airy_cancel_token_t *)AIRY_CALLOC(1, sizeof(airy_cancel_token_t));
    if (!token)
        return AIRY_ERR_OUT_OF_MEMORY;
    if (airy_cancel_token_init(token) != 0) {
        AIRY_FREE(token);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    airy_mtx_lock(&svc->session_lock);
    if (sess_find(svc, request_id)) {

        /* Concurrent invoke with the same request_id: the previous
         * session's token is still polled by another worker thread
         * (agent_read_line_timeout_ex). Destroying it here is a
         * heap-use-after-free (see ASan report on agent_d invoke).
         * The cancel key must be unique per in-flight invoke; reject
         * the duplicate and let the caller fail cleanly. */
        airy_mtx_unlock(&svc->session_lock);
        airy_cancel_token_destroy(token);
        AIRY_FREE(token);
        return AIRY_ERR_BUSY;
    }
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (!s->active) {
            snprintf(s->request_id, sizeof(s->request_id), "%s", request_id);
            s->active = 1;
            s->token = token;
            *out_token = token;
            airy_mtx_unlock(&svc->session_lock);
            return AIRY_SUCCESS;
        }
    }
    airy_mtx_unlock(&svc->session_lock);

    airy_cancel_token_destroy(token);
    AIRY_FREE(token);
    return AIRY_ERR_BUSY;
}

void agent_service_invoke_end(agent_service_t *svc, const char *request_id)
{
    if (!svc || !request_id || request_id[0] == '\0')
        return;
    airy_mtx_lock(&svc->session_lock);
    agent_invoke_session_t *s = sess_find(svc, request_id);
    if (s) {
        s->active = 0;
        s->request_id[0] = '\0';
        if (s->token) {
            airy_cancel_token_destroy(s->token);
            AIRY_FREE(s->token);
            s->token = NULL;
        }
    }
    airy_mtx_unlock(&svc->session_lock);
}

int agent_service_invoke_cancel(agent_service_t *svc, const char *request_id)
{
    if (!svc || !svc->initialized || !request_id || request_id[0] == '\0')
        return AIRY_ERR_INVALID_PARAM;

    airy_cancel_token_t *token = NULL;
    airy_mtx_lock(&svc->session_lock);
    agent_invoke_session_t *s = sess_find(svc, request_id);
    if (s) {
        token = s->token;
        /* Cancel while holding the session lock: the invoke thread's
         * agent_service_invoke_end could otherwise free the token
         * between our unlock and the cancel call (use-after-free). */
        if (token)
            airy_cancel_token_cancel(token);
    }
    airy_mtx_unlock(&svc->session_lock);

    if (!token) {
        SVC_LOG_WARN("agent.cancel: no active invoke session (request_id=%s)", request_id);
        return AIRY_ERR_NOT_FOUND;
    }
    SVC_LOG_INFO("agent.cancel: requested cancel (request_id=%s)", request_id);
    return AIRY_SUCCESS;
}

int agent_service_invoke_cancel_all(agent_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;

    int n = 0;
    /* Same lock discipline as agent_service_invoke_cancel: the token cancel
     * runs under the session lock so the invoke thread's invoke_end cannot
     * free the token between our lookup and the cancel (use-after-free). */
    airy_mtx_lock(&svc->session_lock);
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (s->active && s->token) {
            airy_cancel_token_cancel(s->token);
            n++;
        }
    }
    airy_mtx_unlock(&svc->session_lock);
    return n;
}

void agent_service_invoke_wait_idle(agent_service_t *svc, uint32_t timeout_ms)
{
    if (!svc)
        return;

    uint64_t deadline = (uint64_t)time(NULL) * 1000ull + timeout_ms;
    for (;;) {
        int active = 0;
        airy_mtx_lock(&svc->session_lock);
        for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX && !active; i++)
            active = svc->sessions[i].active;
        airy_mtx_unlock(&svc->session_lock);
        if (!active)
            return;
        if (timeout_ms && (uint64_t)time(NULL) * 1000ull >= deadline) {
            SVC_LOG_WARN("Shutdown drain: invoke sessions still active after %ums", timeout_ms);
            return;
        }
        airy_sleep_ms(100);
    }
}
