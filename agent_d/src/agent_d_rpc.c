// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_d_rpc.c
 * @brief agent.* RPC 方法域：spawn/terminate/invoke/cancel/list/count/
 *        health_check/get_stats，以及 spawn 后的 sched_d 角色注册。
 *
 * 2026-08-27 域拆分（原 main.c 826 行 → 3 文件）：入口引导见 main.c，
 * 空闲回收/性能采样线程见 agent_d_monitor.c。
 */

#include "airy_memory.h"
#include "error.h"
#include "agent_d_internal.h"

#include "daemon_main.h"
#include "agent_service.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static void handle_spawn(cJSON *params, int id, airy_sock_t fd);
static void handle_terminate(cJSON *params, int id, airy_sock_t fd);
static void handle_invoke(cJSON *params, int id, airy_sock_t fd);
static void handle_cancel(cJSON *params, int id, airy_sock_t fd);
static void handle_list(int id, airy_sock_t fd);
static void handle_count(int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);

/*
 * After agent.spawn succeeds, register the role with sched_d, solving the
 * "sched_d registry always empty" problem (historically no component called
 * register_agent, so schedule_task could not select any agent).
 *
 * The registration key is spec.role (e.g. "coding"): sched_d's post-selection
 * dispatch (sched_dispatch_task) passes the registered agent_id directly as
 * the role to agent.spawn, so the registry must store the role to be selected
 * by scheduling. Repeated spawns of the same role only update state (sched_d
 * registration is idempotent: on the same agent_id hit, only the
 * load/availability fields are refreshed).
 *
 * Registration failure only warns and does not block spawn: agent_d can still
 * be called directly by the gateway orchestration branch.
 */
static void sched_d_register_spawned_agent(const char *agent_id, const char *spec_str)
{

    char role[128] = {0};
    cJSON *spec = cJSON_Parse(spec_str);
    if (spec) {
        cJSON *r = cJSON_GetObjectItem(spec, "role");
        if (cJSON_IsString(r) && r->valuestring && *r->valuestring) {
            snprintf(role, sizeof(role), "%s", r->valuestring);
        }
        cJSON_Delete(spec);
    }
    if (role[0] == '\0')
        snprintf(role, sizeof(role), "%s", agent_id ? agent_id : "unknown");

    cJSON *params = cJSON_CreateObject();
    cJSON *agent = cJSON_CreateObject();
    if (!params || !agent) {
        if (params)
            cJSON_Delete(params);
        if (agent)
            cJSON_Delete(agent);
        return;
    }
    cJSON_AddStringToObject(agent, "agent_id", role);
    cJSON_AddStringToObject(agent, "agent_name", role);
    cJSON_AddBoolToObject(agent, "is_available", true);
    cJSON_AddNumberToObject(agent, "weight", 1.0);
    cJSON_AddItemToObject(params, "agent", agent);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return;

    char sock_buf[AIRY_PATH_MAX];
    snprintf(sock_buf, sizeof(sock_buf), "%s", airy_runtime_dir_socket("sched.sock"));
    char *result = NULL;
    int rc = daemon_rpc_call(sock_buf, "register_agent", params_str, &result, 5000);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !result) {
        SVC_LOG_WARN("sched_d register_agent failed (role=%s, agent_id=%s, rc=%d)", role,
                     agent_id ? agent_id : "?", rc);
    } else {
        SVC_LOG_INFO("sched_d registered (role=%s, agent_id=%s)", role, agent_id ? agent_id : "?");
        AIRY_FREE(result);
    }
}

void on_spawn_method(cJSON *params, int id, void *user_data)
{
    handle_spawn(params, id, *(airy_sock_t *)user_data);
}

void on_terminate_method(cJSON *params, int id, void *user_data)
{
    handle_terminate(params, id, *(airy_sock_t *)user_data);
}

void on_invoke_method(cJSON *params, int id, void *user_data)
{
    handle_invoke(params, id, *(airy_sock_t *)user_data);
}

void on_cancel_method(cJSON *params, int id, void *user_data)
{
    handle_cancel(params, id, *(airy_sock_t *)user_data);
}

void on_list_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_list(id, *(airy_sock_t *)user_data);
}

void on_count_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_count(id, *(airy_sock_t *)user_data);
}

void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void handle_spawn(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *spec = cJSON_GetObjectItem(params, "agent_spec");

    if (!spec) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_spec", id);
        return;
    }

    char *spec_str = NULL;
    if (cJSON_IsString(spec)) {
        spec_str = AIRY_STRDUP(spec->valuestring);
    } else {
        spec_str = cJSON_PrintUnformatted(spec);
    }
    if (!spec_str) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid agent_spec", id);
        return;
    }

    uint64_t perf_t0 = perf_now_us();
    char *out_agent_id = NULL;
    int ret = agent_service_spawn(g_service, spec_str, &out_agent_id);

    {
        uint64_t elapsed = perf_now_us() - perf_t0;
        int64_t slow_us = perf_slow_threshold_us();
        if ((int64_t)elapsed > slow_us)
            SVC_LOG_WARN("agent.spawn slow: %llu us (threshold=%lld us)",
                         (unsigned long long)elapsed, (long long)slow_us);
    }

    if (ret != AIRY_SUCCESS || !out_agent_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent spawn failed", id);
        SVC_LOG_ERROR("agent.spawn failed: error=%d", ret);
        AIRY_FREE(spec_str);
        AIRY_FREE(out_agent_id);
        return;
    }

    sched_d_register_spawned_agent(out_agent_id, spec_str);
    AIRY_FREE(spec_str);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "agent_id", out_agent_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(out_agent_id);
}

static void handle_terminate(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    int ret = agent_service_terminate(g_service, agent_id->valuestring);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "terminated", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_invoke(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    cJSON *input = cJSON_GetObjectItem(params, "input");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    const char *input_str = input && cJSON_IsString(input) ? input->valuestring : "";
    size_t input_len = strlen(input_str);

    /* Improvement 1 (cancellation drill-down): an optional request_id param
     * enables a cross-process cancellation session. The caller (wh_agent_invoke
     * etc.) passes a unique request_id; while invoke blocks, the agent.cancel
     * RPC can terminate the child by request_id (SIGTERM->SIGKILL). */
    cJSON *request_id_item = cJSON_GetObjectItem(params, "request_id");
    const char *request_id = (request_id_item && cJSON_IsString(request_id_item) &&
                              request_id_item->valuestring && request_id_item->valuestring[0]) ?
                                 request_id_item->valuestring :
                                 NULL;

    airy_cancel_token_t *token = NULL;
    if (request_id) {
        if (agent_service_invoke_begin(g_service, request_id, &token) != AIRY_SUCCESS) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invoke session register failed",
                               id);
            return;
        }
    }

    uint64_t perf_t0 = perf_now_us();
    char *out_output = NULL;
    cJSON *ws_item = cJSON_GetObjectItem(params, "workspace_dir");
    const char *workspace_dir = (ws_item && cJSON_IsString(ws_item) && ws_item->valuestring &&
                                 ws_item->valuestring[0]) ?
                                    ws_item->valuestring :
                                    NULL;
    int ret = agent_service_invoke(g_service, agent_id->valuestring, input_str, input_len,
                                   workspace_dir, token, &out_output);

    {
        uint64_t elapsed = perf_now_us() - perf_t0;
        int64_t slow_us = perf_slow_threshold_us();
        if ((int64_t)elapsed > slow_us)
            SVC_LOG_WARN("agent.invoke slow: %llu us (threshold=%lld us, agent_id=%s)",
                         (unsigned long long)elapsed, (long long)slow_us, agent_id->valuestring);
    }

    if (request_id)
        agent_service_invoke_end(g_service, request_id);

    if (ret == AIRY_SUCCESS && out_output) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "output", out_output);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        AIRY_FREE(out_output);
        return;
    }

    AIRY_FREE(out_output);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
    } else if (ret == AIRY_ERR_CANCELED) {

        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent invoke canceled", id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent invoke failed", id);
    }
    SVC_LOG_ERROR("agent.invoke failed: error=%d", ret);
}

/* Improvement 1 (cancellation drill-down): agent.cancel — cancel the active
 * invoke session by request_id. Cross-process cancel chain: the caller
 * daemon_rpc_call_cancelable sends this method when the cancel token hits ->
 * the service layer senses the token via select polling -> SIGTERM->2s->SIGKILL
 * the child -> the original invoke returns with AbortedOutput (the caller
 * distinguishes "cancel" from "timeout" by this). */
static void handle_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *request_id = cJSON_GetObjectItem(params, "request_id");

    if (!request_id || !cJSON_IsString(request_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing request_id", id);
        return;
    }

    int ret = agent_service_invoke_cancel(g_service, request_id->valuestring);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "No active invoke for request_id",
                           id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "canceled", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_list(int id, airy_sock_t client_fd)
{
    char **agent_ids = NULL;
    size_t count = 0;

    int ret = agent_service_list(g_service, &agent_ids, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent list failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(agent_ids[i]));
    }
    cJSON_AddItemToObject(result, "agent_ids", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    agent_service_list_free(agent_ids, count);
}

static void handle_count(int id, airy_sock_t client_fd)
{
    size_t n = agent_service_count(g_service);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    bool healthy = g_service != NULL;
    size_t agents = healthy ? agent_service_count(g_service) : 0;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "agent_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddNumberToObject(result, "agents", (double)agents);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "agent_d");
    cJSON_AddNumberToObject(result, "uptime_s", (double)((uint64_t)time(NULL) - g_start_time));
    if (g_service) {
        cJSON_AddNumberToObject(result, "agents", (double)agent_service_count(g_service));
        agent_perf_stats_t perf;
        if (agent_service_get_perf(g_service, &perf) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "spawn_total", perf.spawn_total);
            cJSON_AddNumberToObject(result, "spawn_ok", perf.spawn_ok);
            cJSON_AddNumberToObject(result, "spawn_fail", perf.spawn_fail);
            cJSON_AddNumberToObject(result, "invoke_total", perf.invoke_total);
            cJSON_AddNumberToObject(result, "invoke_ok", perf.invoke_ok);
            cJSON_AddNumberToObject(result, "invoke_fail", perf.invoke_fail);
            cJSON_AddNumberToObject(result, "terminate_total", perf.terminate_total);
            cJSON_AddNumberToObject(result, "lock_wait_total", perf.lock_wait_total);
            cJSON_AddNumberToObject(result, "peak_running", perf.peak_running);
            if (perf.spawn_ok > 0) {
                cJSON_AddNumberToObject(result, "avg_spawn_us",
                                        (double)perf.spawn_us_total / (double)perf.spawn_ok);
            }
            if (perf.invoke_ok > 0) {
                cJSON_AddNumberToObject(result, "avg_invoke_us",
                                        (double)perf.invoke_us_total / (double)perf.invoke_ok);
            }
        }
    } else {
        cJSON_AddNumberToObject(result, "agents", 0);
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
