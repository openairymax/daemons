// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_invoke.c
 * @brief Agent 服务调用域：agent 终止/调用（子进程 stdin/stdout 管道
 *        协议）与 invoke 会话注册/注销/取消管理
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
#include <sys/select.h>
#include <sys/wait.h>
#endif

#include "agent_service_internal.h"

int agent_service_terminate(agent_service_t *svc, const char *agent_id)
{
    if (!svc || !svc->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_atomic_fetch_add(&svc->m_terminate_total, 1);

    agent_lock_svc(svc);

    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    agent_entry_internal_t *agent = &svc->agents[idx];
    airy_mtx_unlock(&svc->lock);

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

int agent_service_invoke(agent_service_t *svc, const char *agent_id, const char *input, size_t len,
                         airy_cancel_token_t *cancel_token, char **out_output)
{
    if (!svc || !svc->initialized || !agent_id || !out_output)
        return AIRY_ERR_INVALID_PARAM;

    *out_output = NULL;

    uint64_t perf_t0 = agent_perf_now_us();
    airy_atomic_fetch_add(&svc->m_invoke_total, 1);

    agent_lock_svc(svc);
    ssize_t idx = agent_ht_lookup(&svc->agent_index, agent_id);
    if (idx < 0 || (size_t)idx >= svc->agent_count) {
        airy_mtx_unlock(&svc->lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not found\"}");
        airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
        return AIRY_ERR_NOT_FOUND;
    }
    agent_entry_internal_t *agent = &svc->agents[idx];
    airy_mtx_unlock(&svc->lock);

    /* Child communication runs under the fine-grained lock: calls to the same
     * Agent are serialized, calls to different Agents do not block each other,
     * supporting thousands of Agents running in parallel */
    airy_mtx_lock(&agent->entry_lock);

    if (agent->status != AGENT_STATUS_RUNNING) {
        airy_mtx_unlock(&agent->entry_lock);
        *out_output = AIRY_STRDUP("{\"error\":\"Agent not running\"}");
        airy_atomic_fetch_add(&svc->m_invoke_fail, 1);
        return AIRY_ERR_STATE_ERROR;
    }

#if AIRY_PLATFORM_POSIX
    /* Stage5+ todo4: real child-process path — communication via stdin/stdout
     * pipes. Protocol: write one JSON request line to the child stdin, read one
     * JSON response line from stdout. child_pid>0 and stdin_fd>=0 mean an
     * active child; otherwise fall back. */
    if (agent->child_pid > 0 && agent->stdin_fd >= 0) {
        int sin_fd = agent->stdin_fd;
        int sout_fd = agent->stdout_fd;

        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "agent_id", agent_id);
        cJSON_AddStringToObject(req, "input", input ? input : "");
        char *req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);

        if (!req_str) {
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP("{\"error\":\"failed to build request\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }

        size_t req_len = strlen(req_str);
        char *write_buf = (char *)AIRY_MALLOC(req_len + 2);
        if (!write_buf) {
            AIRY_FREE(req_str);
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(write_buf, req_str, req_len);
        write_buf[req_len] = '\n';
        write_buf[req_len + 1] = '\0';
        AIRY_FREE(req_str);

        int wrc = agent_write_all(sin_fd, write_buf, req_len + 1);
        AIRY_FREE(write_buf);
        if (wrc != 0) {
            SVC_LOG_WARN("Agent invoke write failed, child unusable: agent_id=%s", agent_id);
            agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
            goto invoke_fallback;
        }

        agent->last_active = (uint64_t)time(NULL);

        char *resp_buf = (char *)AIRY_MALLOC(AGENT_RESP_BUF_SIZE);
        if (!resp_buf) {
            airy_mtx_unlock(&agent->entry_lock);
            *out_output = AIRY_STRDUP("{\"error\":\"out of memory\"}");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        int rrc = agent_read_line_timeout_ex(sout_fd, resp_buf, AGENT_RESP_BUF_SIZE,
                                             agent_invoke_timeout_s(), cancel_token);
        if (rrc == -2) {
            /* Cancel: gracefully terminate the child (SIGTERM->2s->SIGKILL on
             * the process group), finishing with AbortedOutput, clearly
             * distinguished from the timeout (fallback) path */
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
            goto invoke_fallback;
        }

        agent->last_active = (uint64_t)time(NULL);

        /* rrc==1: the response exceeded AGENT_RESP_BUF_SIZE and was truncated;
         * append an explicit marker to avoid silent data loss (callers can
         * detect the \n...[truncated] suffix) */
        if (rrc == 1) {
            static const char TRUNC_SUFFIX[] = "\n...[agent response truncated]";
            size_t slen = strlen(resp_buf);
            size_t avail = AGENT_RESP_BUF_SIZE - slen - 1;
            if (avail >= sizeof(TRUNC_SUFFIX) - 1)
                AIRY_MEMCPY(resp_buf + slen, TRUNC_SUFFIX, sizeof(TRUNC_SUFFIX));
            else if (avail > 1)
                AIRY_MEMCPY(resp_buf + slen, TRUNC_SUFFIX, avail - 1);
            resp_buf[AGENT_RESP_BUF_SIZE - 1] = '\0';
            SVC_LOG_WARN("Agent invoke response truncated at %d bytes (agent_id=%s)",
                         (int)AGENT_RESP_BUF_SIZE, agent_id);
        }

        /* Parse the response JSON and extract the output field (runner.py
         * convention). Success: {"success":true,"output":"..."}
         * Failure: {"success":false,"error":"..."} */
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON *success_item = cJSON_GetObjectItem(resp, "success");
            cJSON *err_item = cJSON_GetObjectItem(resp, "error");
            if (success_item && cJSON_IsFalse(success_item) && err_item &&
                cJSON_IsString(err_item)) {
                *out_output = AIRY_STRDUP(err_item->valuestring);
            } else {
                cJSON *output_item = cJSON_GetObjectItem(resp, "output");
                if (output_item && cJSON_IsString(output_item)) {
                    *out_output = AIRY_STRDUP(output_item->valuestring);
                } else {
                    *out_output = AIRY_STRDUP(resp_buf);
                }
            }
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

invoke_fallback:
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
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (s->active && strcmp(s->request_id, request_id) == 0) {

            s->active = 0;
            if (s->token) {
                airy_cancel_token_destroy(s->token);
                AIRY_FREE(s->token);
                s->token = NULL;
            }
        }
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
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (s->active && strcmp(s->request_id, request_id) == 0) {
            s->active = 0;
            s->request_id[0] = '\0';
            if (s->token) {
                airy_cancel_token_destroy(s->token);
                AIRY_FREE(s->token);
                s->token = NULL;
            }
            break;
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
    for (size_t i = 0; i < AGENT_INVOKE_SESSIONS_MAX; i++) {
        agent_invoke_session_t *s = &svc->sessions[i];
        if (s->active && strcmp(s->request_id, request_id) == 0) {
            token = s->token;
            break;
        }
    }
    airy_mtx_unlock(&svc->session_lock);

    if (!token) {
        SVC_LOG_WARN("agent.cancel: no active invoke session (request_id=%s)", request_id);
        return AIRY_ERR_NOT_FOUND;
    }
    airy_cancel_token_cancel(token);
    SVC_LOG_INFO("agent.cancel: requested cancel (request_id=%s)", request_id);
    return AIRY_SUCCESS;
}
