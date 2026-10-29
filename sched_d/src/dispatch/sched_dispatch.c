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

static void agent_socket(char *buf, size_t size)
{
    const char *env = getenv("AIRY_SCHED_AGENT_SOCK");
    if (env && env[0] != '\0') {
        snprintf(buf, size, "%s", env);
    } else {
        snprintf(buf, size, "%s/agent.sock", airy_runtime_dir());
    }
}

static uint32_t timeout_ms(void)
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
/* 调一次 agent.sock RPC 并取回顶层字符串字段的堆副本；任一环节失败返回
 * 非零错误码，*out 保持 NULL。 */
static int disp_call_str(const char *sock, const char *method, const char *params,
                         const char *field, char **out)
{
    char *res = NULL;
    int rc = daemon_rpc_call(sock, method, params, &res, timeout_ms());
    if (rc != AIRY_SUCCESS || !res) {
        SVC_LOG_ERROR("sched dispatch: agent.%s failed (rc=%d)", method, rc);
        AIRY_FREE(res);
        return AIRY_ERR_SVC_NOT_READY;
    }

    cJSON *root = cJSON_Parse(res);
    AIRY_FREE(res);
    if (!root) {
        SVC_LOG_ERROR("sched dispatch: agent.%s result parse failed", method);
        return AIRY_ERR_PARSE_ERROR;
    }
    cJSON *v = cJSON_GetObjectItem(root, field);
    *out = (cJSON_IsString(v) && v->valuestring) ? AIRY_STRDUP(v->valuestring) : NULL;
    cJSON_Delete(root);
    if (!*out) {
        SVC_LOG_ERROR("sched dispatch: agent.%s result missing %s", method, field);
        return AIRY_ERR_STATE_ERROR;
    }
    return AIRY_SUCCESS;
}

/* 尽力回收 agent 进程：失败只告警，不影响已取回的任务输出。 */
static void disp_terminate(const char *sock, const char *agent_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "agent_id", agent_id);
    char *text = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!text)
        return;
    char *res = NULL;
    int rc = daemon_rpc_call(sock, "terminate", text, &res, timeout_ms());
    AIRY_FREE(text);
    AIRY_FREE(res);
    if (rc != AIRY_SUCCESS)
        SVC_LOG_WARN("sched dispatch: agent.terminate failed (agent=%s, rc=%d)", agent_id, rc);
}

static int sched_dispatch_task(const char *role, const char *task_description,
                               const char *workspace_dir, sched_dispatch_result_t *out_result)
{
    if (!role || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    char sock[AIRY_PATH_MAX];
    agent_socket(sock, sizeof(sock));

    /* B7：不硬编码 language——未声明时由 agent_d 缺省兜底（python）。 */
    cJSON *spec = cJSON_CreateObject();
    cJSON_AddStringToObject(spec, "role", role);
    char *spec_str = cJSON_PrintUnformatted(spec);
    cJSON_Delete(spec);

    cJSON *spawn = cJSON_CreateObject();
    if (spec_str)
        cJSON_AddStringToObject(spawn, "agent_spec", spec_str);
    char *spawn_str = spec_str ? cJSON_PrintUnformatted(spawn) : NULL;
    cJSON_Delete(spawn);
    AIRY_FREE(spec_str);

    char *agent_id = NULL;
    int rc = spawn_str ? disp_call_str(sock, "spawn", spawn_str, "agent_id", &agent_id)
                       : AIRY_ERR_OUT_OF_MEMORY;
    AIRY_FREE(spawn_str);
    if (rc != AIRY_SUCCESS)
        return rc;
    /* 空 agent_id 视同缺失：后续 invoke 无从定位目标进程。 */
    if (!agent_id[0]) {
        AIRY_FREE(agent_id);
        return AIRY_ERR_STATE_ERROR;
    }

    cJSON *invoke = cJSON_CreateObject();
    cJSON_AddStringToObject(invoke, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke, "input", task_description ? task_description : "");
    if (workspace_dir && workspace_dir[0])
        cJSON_AddStringToObject(invoke, "workspace_dir", workspace_dir);
    char *invoke_str = cJSON_PrintUnformatted(invoke);
    cJSON_Delete(invoke);

    char *output = NULL;
    rc = invoke_str ? disp_call_str(sock, "invoke", invoke_str, "output", &output)
                    : AIRY_ERR_OUT_OF_MEMORY;
    AIRY_FREE(invoke_str);
    if (rc != AIRY_SUCCESS) {
        AIRY_FREE(agent_id);
        return rc;
    }

    disp_terminate(sock, agent_id);
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
