// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dispatch.c
 * @brief Scheduler daemon - real dispatch (agent_d interaction) domain.
 * @details Implements the P2.2 real dispatch chain over agent_d's Unix
 *          socket (spawn + invoke + terminate) and the sched_dispatch_executor
 *          callback injected into the scheduler service. sched_dispatch_executor
 *          was promoted from static when main.c was split by functional
 *          domain (main.c passes it to sched_service_set_executor;
 *          declaration in sched_daemon_internal.h); the dispatch helpers
 *          remain static here.
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "sched_daemon_internal.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

typedef struct {
    char *agent_id;
    char *output;
} sched_dispatch_result_t;
static int sched_dispatch_enabled(void);
static int sched_dispatch_task(const char *role, const char *task_description,
                               const char *workspace_dir, sched_dispatch_result_t *out_result);

static int sched_dispatch_enabled(void)
{
    const char *env = getenv("AIRY_SCHED_DISPATCH");
    return !(env && env[0] != '\0' && strcmp(env, "0") == 0);
}

static void sched_dispatch_agent_socket(char *buf, size_t size)
{
    const char *env = getenv("AIRY_SCHED_AGENT_SOCK");
    if (env && env[0] != '\0') {
        snprintf(buf, size, "%s", env);
    } else {
        snprintf(buf, size, "%s/agent.sock", airy_runtime_dir());
    }
}

static uint32_t sched_dispatch_timeout_ms(void)
{
    const char *env = getenv("AIRY_SCHED_DISPATCH_TIMEOUT_MS");
    if (env && env[0] != '\0') {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0)
            return (uint32_t)v;
    }
    return 300000;
}

/*
 * P2.2 real dispatch: after scheduling picks a role, call agent.spawn +
 * agent.invoke over agent_d's Unix socket so the real Agent child process
 * (Python runner -> LLM) for that role executes the task and returns real
 * output. On failure, *out_result is not set and a non-zero error code is
 * returned — the caller must report it faithfully; fake data must never
 * substitute for real execution (following the gateway syscall_router
 * agent.sock call pattern).
 */
static int sched_dispatch_task(const char *role, const char *task_description,
                               const char *workspace_dir, sched_dispatch_result_t *out_result)
{
    if (!role || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    char sock[AIRY_PATH_MAX];
    sched_dispatch_agent_socket(sock, sizeof(sock));

    cJSON *spec = cJSON_CreateObject();
    cJSON_AddStringToObject(spec, "role", role);
    cJSON_AddStringToObject(spec, "language", "python");
    char *spec_str = cJSON_PrintUnformatted(spec);
    cJSON_Delete(spec);
    if (!spec_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    cJSON *spawn_params = cJSON_CreateObject();
    cJSON_AddStringToObject(spawn_params, "agent_spec", spec_str);
    char *spawn_params_str = cJSON_PrintUnformatted(spawn_params);
    cJSON_Delete(spawn_params);
    AIRY_FREE(spec_str);
    if (!spawn_params_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *spawn_result = NULL;
    int rc = daemon_rpc_call(sock, "spawn", spawn_params_str, &spawn_result,
                             sched_dispatch_timeout_ms());
    AIRY_FREE(spawn_params_str);
    if (rc != AIRY_SUCCESS || !spawn_result) {
        SVC_LOG_ERROR("sched dispatch: agent.spawn failed (role=%s, rc=%d)", role, rc);
        if (spawn_result)
            AIRY_FREE(spawn_result);
        return AIRY_ERR_SVC_NOT_READY;
    }

    cJSON *spawn_root = cJSON_Parse(spawn_result);
    AIRY_FREE(spawn_result);
    if (!spawn_root) {
        SVC_LOG_ERROR("sched dispatch: spawn result parse failed (role=%s)", role);
        return AIRY_ERR_PARSE_ERROR;
    }
    cJSON *aid = cJSON_GetObjectItem(spawn_root, "agent_id");
    if (!aid || !cJSON_IsString(aid) || !aid->valuestring || !aid->valuestring[0]) {
        SVC_LOG_ERROR("sched dispatch: spawn result missing agent_id (role=%s)", role);
        cJSON_Delete(spawn_root);
        return AIRY_ERR_STATE_ERROR;
    }
    char *agent_id = AIRY_STRDUP(aid->valuestring);
    cJSON_Delete(spawn_root);
    if (!agent_id)
        return AIRY_ERR_OUT_OF_MEMORY;

    cJSON *invoke_params = cJSON_CreateObject();
    cJSON_AddStringToObject(invoke_params, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke_params, "input", task_description ? task_description : "");
    if (workspace_dir && workspace_dir[0])
        cJSON_AddStringToObject(invoke_params, "workspace_dir", workspace_dir);
    char *invoke_params_str = cJSON_PrintUnformatted(invoke_params);
    cJSON_Delete(invoke_params);
    if (!invoke_params_str) {
        AIRY_FREE(agent_id);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    char *invoke_result = NULL;
    rc = daemon_rpc_call(sock, "invoke", invoke_params_str, &invoke_result,
                         sched_dispatch_timeout_ms());
    AIRY_FREE(invoke_params_str);
    if (rc != AIRY_SUCCESS || !invoke_result) {
        SVC_LOG_ERROR("sched dispatch: agent.invoke failed (agent=%s, rc=%d)", agent_id, rc);
        AIRY_FREE(agent_id);
        if (invoke_result)
            AIRY_FREE(invoke_result);
        return AIRY_ERR_SVC_NOT_READY;
    }

    cJSON *invoke_root = cJSON_Parse(invoke_result);
    AIRY_FREE(invoke_result);
    if (!invoke_root) {
        SVC_LOG_ERROR("sched dispatch: invoke result parse failed (agent=%s)", agent_id);
        AIRY_FREE(agent_id);
        return AIRY_ERR_PARSE_ERROR;
    }
    cJSON *out = cJSON_GetObjectItem(invoke_root, "output");
    if (!out || !cJSON_IsString(out) || !out->valuestring) {
        SVC_LOG_ERROR("sched dispatch: invoke result missing output (agent=%s)", agent_id);
        cJSON_Delete(invoke_root);
        AIRY_FREE(agent_id);
        return AIRY_ERR_STATE_ERROR;
    }
    char *output = AIRY_STRDUP(out->valuestring);
    cJSON_Delete(invoke_root);
    if (!output) {
        AIRY_FREE(agent_id);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    {
        cJSON *term_params = cJSON_CreateObject();
        cJSON_AddStringToObject(term_params, "agent_id", agent_id);
        char *term_params_str = cJSON_PrintUnformatted(term_params);
        cJSON_Delete(term_params);
        if (term_params_str) {
            char *term_result = NULL;
            int trc = daemon_rpc_call(sock, "terminate", term_params_str, &term_result,
                                      sched_dispatch_timeout_ms());
            AIRY_FREE(term_params_str);
            if (term_result)
                AIRY_FREE(term_result);
            if (trc != AIRY_SUCCESS)
                SVC_LOG_WARN("sched dispatch: agent.terminate failed (agent=%s, rc=%d)", agent_id,
                             trc);
        }
    }

    out_result->agent_id = agent_id;
    out_result->output = output;
    SVC_LOG_INFO("sched dispatch: role=%s agent=%s dispatched (output_len=%zu)", role, agent_id,
                 strlen(output));
    return AIRY_SUCCESS;
}

/*
 * Task execution callback (injected into sched_service, called by the worker
 * thread): reuses the real chain of sched_dispatch_task (agent selection is
 * already done by sched_service; here spawn+invoke+terminate run for the
 * selected role).
 */
int sched_dispatch_executor(const char *agent_id, const char *task_description,
                            const char *workspace_dir, char **out_output)
{

    if (!sched_dispatch_enabled()) {
        SVC_LOG_WARN("sched dispatch disabled (AIRY_SCHED_DISPATCH=0), task not executed");
        return AIRY_ERR_NOT_SUPPORTED;
    }

    sched_dispatch_result_t dispatch = {0};
    int dret = sched_dispatch_task(agent_id, task_description, workspace_dir, &dispatch);
    if (dret != AIRY_SUCCESS || !dispatch.output || !dispatch.agent_id) {
        AIRY_FREE(dispatch.agent_id);
        AIRY_FREE(dispatch.output);
        return dret;
    }
    *out_output = dispatch.output;
    AIRY_FREE(dispatch.agent_id);
    return AIRY_SUCCESS;
}
