// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Scheduler service daemon main entry (daemon module conventions).
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 */

#include "../../monit_d/include/monitor_service.h"
#include "daemon_main.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "param_validator.h"
#include "scheduler_service.h"
#include "strategy_interface.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("sched.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_sched"
#define DEFAULT_TCP_PORT 8083
#define MAX_BUFFER 65536

DAEMON_DECLARE_COMMON(sched_d, scheduler, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(sched_d)

static sched_service_t *g_service = NULL;

#define SCHED_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define SCHED_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define SCHED_ERR_NOT_FOUND AIRY_ERR_NOT_FOUND
#define SCHED_ERR_INVALID_CONFIG (AIRY_ERR_DAEMON_BASE + 0x01)
#define SCHED_ERR_STRATEGY_FAIL (AIRY_ERR_DAEMON_BASE + 0x02)

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_unregister_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_schedule_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_cancel_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_submit(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_status(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_cancel(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_stats(int id, airy_sock_t client_fd);
static void handle_health_check(int id, airy_sock_t client_fd);
static void handle_checkpoint_save(cJSON *params, int id, airy_sock_t client_fd);

typedef struct {
    char *agent_id;
    char *output;
} sched_dispatch_result_t;
static int sched_dispatch_enabled(void);
static int sched_dispatch_task(const char *role, const char *task_description,
                               sched_dispatch_result_t *out_result);

static void on_register_agent_method(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_unregister_agent_method(cJSON *params, int id, void *user_data)
{
    handle_unregister_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_schedule_task_method(cJSON *params, int id, void *user_data)
{
    handle_schedule_task(params, id, *(airy_sock_t *)user_data);
}

static void on_get_task_method(cJSON *params, int id, void *user_data)
{
    handle_get_task(params, id, *(airy_sock_t *)user_data);
}

static void on_cancel_task_method(cJSON *params, int id, void *user_data)
{
    handle_cancel_task(params, id, *(airy_sock_t *)user_data);
}

static void on_dag_submit_method(cJSON *params, int id, void *user_data)
{
    handle_dag_submit(params, id, *(airy_sock_t *)user_data);
}

static void on_dag_status_method(cJSON *params, int id, void *user_data)
{
    handle_dag_status(params, id, *(airy_sock_t *)user_data);
}

static void on_dag_cancel_method(cJSON *params, int id, void *user_data)
{
    handle_dag_cancel(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

static void on_checkpoint_save_method(cJSON *params, int id, void *user_data)
{
    handle_checkpoint_save(params, id, *(airy_sock_t *)user_data);
}

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_json = jsonrpc_get_object_param(params, "agent");
    if (!agent_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent object", id);
        return;
    }

    agent_info_t info = {0};
    const char *aid = get_string_field(agent_json, "agent_id", NULL);
    if (!aid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }
    /* agent_id/agent_name are heap-pointer fields: must be allocated with
     * AIRY_STRDUP. The original AIRY_STRNCPY_TERM(…, sizeof(char*)) wrote 7
     * bytes to a NULL pointer, inevitably SEGV (a pre-existing P0 defect,
     * fixed here). */
    info.agent_id = AIRY_STRDUP(aid);
    const char *aname = get_string_field(agent_json, "agent_name", NULL);
    info.agent_name = AIRY_STRDUP(aname ? aname : "");
    if (!info.agent_id || !info.agent_name) {
        AIRY_FREE(info.agent_id);
        AIRY_FREE(info.agent_name);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    info.load_factor = get_double_field(agent_json, "load_factor", 0.0);
    info.success_rate = get_double_field(agent_json, "success_rate", 0.0);
    info.avg_response_time_ms = get_int_field(agent_json, "avg_response_time_ms", 0);
    info.is_available = get_bool_field(agent_json, "is_available", false);
    info.weight = get_double_field(agent_json, "weight", 1.0);

    int ret = sched_service_register_agent(g_service, &info);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register agent: %s (error=%d)", info.agent_id, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "registered");
        cJSON_AddStringToObject(result, "agent_id", info.agent_id);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Agent registered: %s", info.agent_id);
    }

    AIRY_FREE(info.agent_id);
    AIRY_FREE(info.agent_name);
}

static void handle_unregister_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *aid_json = cJSON_GetObjectItem(params, "agent_id");
    if (!cJSON_IsString(aid_json) || !aid_json->valuestring || !*aid_json->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    int ret = sched_service_unregister_agent(g_service, aid_json->valuestring);
    if (ret != AIRY_SUCCESS && ret != AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Unregister failed", id);
        SVC_LOG_ERROR("Failed to unregister agent: %s (error=%d)", aid_json->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "unregistered");
    cJSON_AddStringToObject(result, "agent_id", aid_json->valuestring);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("Agent unregistered: %s", aid_json->valuestring);
}

static void handle_schedule_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_json = jsonrpc_get_object_param(params, "task");
    if (!task_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task object", id);
        return;
    }

    task_info_t task = {0};

    const char *tid = get_string_field(task_json, "task_id", NULL);
    task.task_id = tid ? AIRY_STRDUP(tid) : NULL;
    const char *desc = get_string_field(task_json, "task_description", NULL);
    task.task_description = AIRY_STRDUP(desc ? desc : "");
    if (!task.task_description) {
        AIRY_FREE(task.task_id);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    /* Empty-task guard: refuse to enqueue when both task_id and
     * task_description are missing, preventing meaningless empty tasks from
     * entering the async queue (the worker cannot select an agent/execute
     * from them) */
    if ((!tid || !*tid) && (!desc || !*desc)) {
        AIRY_FREE(task.task_id);
        AIRY_FREE(task.task_description);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "task_id or task_description required", id);
        return;
    }

    task.priority = get_int_field(task_json, "priority", 0);
    task.timeout_ms = get_int_field(task_json, "timeout_ms", 30000);

    /* Async enqueue: return task_id + status=pending immediately; the worker
     * thread later completes select agent -> spawn -> invoke -> status
     * write-back (queried via get_task). */
    char *assigned_id = NULL;
    int ret = sched_service_submit_task(g_service, &task, &assigned_id);
    AIRY_FREE(task.task_id);
    AIRY_FREE(task.task_description);
    if (ret != AIRY_SUCCESS || !assigned_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Task enqueue failed", id);
        SVC_LOG_ERROR("Task enqueue failed: error=%d", ret);
        AIRY_FREE(assigned_id);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "task_id", assigned_id);
    cJSON_AddStringToObject(res_obj, "status", "pending");
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("Task scheduled (async): %s", assigned_id);
    AIRY_FREE(assigned_id);
}

static void handle_get_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *tid = cJSON_GetObjectItem(params, "task_id");
    if (!cJSON_IsString(tid) || !tid->valuestring || !*tid->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    char *json_out = NULL;
    int ret = sched_service_get_task(g_service, tid->valuestring, &json_out);
    if (ret != AIRY_SUCCESS || !json_out) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           ret == AIRY_ERR_NOT_FOUND ? "Task not found" : "Get task failed", id);
        return;
    }

    CJSON_PARSE_GUARD(report_json, json_out, {
        AIRY_FREE(json_out);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid task data", id);
        return;
    });
    AIRY_FREE(json_out);

    JSONRPC_SEND_SUCCESS(client_fd, report_json, id);
    report_json = NULL;
}

static void handle_cancel_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *tid = cJSON_GetObjectItem(params, "task_id");
    if (!cJSON_IsString(tid) || !tid->valuestring || !*tid->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    int ret = sched_service_cancel_task(g_service, tid->valuestring);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Task not found", id);
        return;
    }
    if (ret == AIRY_ERR_BUSY) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Task not cancelable", id);
        return;
    }
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cancel failed", id);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "task_id", tid->valuestring);
    cJSON_AddStringToObject(res_obj, "status", "canceled");
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("Task canceled via RPC: %s", tid->valuestring);
}

static void handle_dag_submit(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *dag_json = jsonrpc_get_object_param(params, "dag");
    if (!dag_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing dag object", id);
        return;
    }
    char *dag_str = cJSON_PrintUnformatted(dag_json);
    if (!dag_str) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "JSON serialization failed", id);
        return;
    }

    char *dag_id = NULL;
    int ret = sched_service_submit_dag(g_service, dag_str, &dag_id);
    AIRY_FREE(dag_str);
    if (ret == AIRY_ERR_CYCLE_DETECTED) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "DAG cycle detected", id);
        return;
    }
    if (ret == AIRY_ERR_OVERFLOW) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "DAG exceeds capacity limits", id);
        return;
    }
    if (ret != AIRY_SUCCESS || !dag_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "DAG submit failed", id);
        AIRY_FREE(dag_id);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "dag_id", dag_id);
    cJSON_AddStringToObject(res_obj, "status", "active");
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("DAG submitted via RPC: %s", dag_id);
    AIRY_FREE(dag_id);
}

static void handle_dag_status(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *did = cJSON_GetObjectItem(params, "dag_id");
    if (!cJSON_IsString(did) || !did->valuestring || !*did->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing dag_id", id);
        return;
    }

    char *json_out = NULL;
    int ret = sched_service_get_dag(g_service, did->valuestring, &json_out);
    if (ret != AIRY_SUCCESS || !json_out) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           ret == AIRY_ERR_NOT_FOUND ? "DAG not found" : "Get DAG failed", id);
        return;
    }

    CJSON_PARSE_GUARD(report_json, json_out, {
        AIRY_FREE(json_out);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid DAG data", id);
        return;
    });
    AIRY_FREE(json_out);

    JSONRPC_SEND_SUCCESS(client_fd, report_json, id);
    report_json = NULL;
}

static void handle_dag_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *did = cJSON_GetObjectItem(params, "dag_id");
    if (!cJSON_IsString(did) || !did->valuestring || !*did->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing dag_id", id);
        return;
    }

    int ret = sched_service_cancel_dag(g_service, did->valuestring);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "DAG not found", id);
        return;
    }
    if (ret == AIRY_ERR_BUSY) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "DAG not active", id);
        return;
    }
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "DAG cancel failed", id);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "dag_id", did->valuestring);
    cJSON_AddStringToObject(res_obj, "status", "canceled");
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("DAG canceled via RPC: %s", did->valuestring);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    void *stats_data = NULL;
    int ret = sched_service_get_stats(g_service, &stats_data);

    if (ret != AIRY_SUCCESS || !stats_data) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Get stats failed", id);
        return;
    }

    CJSON_PARSE_GUARD(report_json, (char *)stats_data, {
        AIRY_FREE(stats_data);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid report data", id);
        return;
    });
    AIRY_FREE(stats_data);

    JSONRPC_SEND_SUCCESS(client_fd, report_json, id);
    report_json = NULL;
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    bool healthy = false;
    (void)sched_service_health_check(g_service, &healthy);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "sched_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_checkpoint_save(cJSON *params __attribute__((unused)), int id,
                                   airy_sock_t client_fd)
{
    char *json_out = NULL;
    int ret = sched_service_checkpoint_save(g_service, &json_out);
    if (ret != AIRY_SUCCESS || !json_out) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Checkpoint save failed", id);
        return;
    }

    CJSON_PARSE_GUARD(report_json, json_out, {
        AIRY_FREE(json_out);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid checkpoint data", id);
        return;
    });
    AIRY_FREE(json_out);

    JSONRPC_SEND_SUCCESS(client_fd, report_json, id);
    report_json = NULL;
}

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
                               sched_dispatch_result_t *out_result)
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
static int sched_dispatch_executor(const char *agent_id, const char *task_description,
                                   char **out_output)
{

    if (!sched_dispatch_enabled()) {
        SVC_LOG_WARN("sched dispatch disabled (AIRY_SCHED_DISPATCH=0), task not executed");
        return AIRY_ERR_NOT_SUPPORTED;
    }

    sched_dispatch_result_t dispatch = {0};
    int dret = sched_dispatch_task(agent_id, task_description, &dispatch);
    if (dret != AIRY_SUCCESS || !dispatch.output || !dispatch.agent_id) {
        AIRY_FREE(dispatch.agent_id);
        AIRY_FREE(dispatch.output);
        return dret;
    }
    *out_output = dispatch.output;
    AIRY_FREE(dispatch.agent_id);
    return AIRY_SUCCESS;
}

static void destroy_service(void)
{
    if (g_service) {
        sched_service_destroy(g_service);
        g_service = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = "agentrt/manager/service/sched_d/sched.yaml";
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_sched_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_sched_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_sched_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(sched_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("sched_d");

    SVC_LOG_INFO("Scheduler service starting, manager=%s", config_path);

    sched_config_t config = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                             .health_check_interval_ms = 5000,
                             .stats_report_interval_ms = 10000,
                             .enable_ml_strategy = false,
                             .ml_model_path = NULL,
                             .max_agents = 100,
                             /* DAG parallel dispatch: 0 = serial (default,
                              * keeps legacy behavior). Env AIRY_DAG_PARALLEL=N
                              * (N>=1) enables mac_framework delegation-mode
                              * parallelism with N as the concurrency cap. */
                             .dag_max_parallel = 0,
                             .dag_batch_size = 0,
                             /* Failure-grading semantics (improvement 3):
                              * production defaults to only FATAL cascading
                              * graph cancellation; ordinary failures do not
                              * interrupt independent branches.
                              * AIRY_DAG_FATAL_CASCADE=0 restores legacy. */
                             .dag_fatal_cascade = true};
    {
        const char *dag_fc = getenv("AIRY_DAG_FATAL_CASCADE");
        if (dag_fc && dag_fc[0] != '\0' && strcmp(dag_fc, "0") == 0) {
            config.dag_fatal_cascade = false;
            SVC_LOG_WARN("sched: DAG fatal-cascade disabled "
                         "(AIRY_DAG_FATAL_CASCADE=0) — any node failure aborts graph");
        }
    }
    {
        const char *dag_par = getenv("AIRY_DAG_PARALLEL");
        if (dag_par && dag_par[0] != '\0') {
            unsigned long pv = strtoul(dag_par, NULL, 10);
            if (pv > 0 && pv <= SCHED_DAG_MAX_NODES) {
                config.dag_max_parallel = (uint32_t)pv;
                SVC_LOG_INFO("sched: DAG parallel mode enabled via AIRY_DAG_PARALLEL=%lu", pv);
            } else {
                SVC_LOG_WARN("sched: invalid AIRY_DAG_PARALLEL=%s (1..%d), fallback serial",
                             dag_par, SCHED_DAG_MAX_NODES);
            }
        }
    }

    int ret = sched_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create scheduler service (error=%d)", ret);

        goto out_mtx_sock;
    }

    SVC_LOG_INFO("Scheduler service created with strategy: round_robin");

    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX,
                                    DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");

        goto out_service;
    }
    SVC_LOG_INFO(use_tcp ? "Listening on TCP port %d" : "Listening on Unix socket",
                 DEFAULT_TCP_PORT);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_sched_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : DEFAULT_SOCKET_PATH_UNIX;
    ret =
        daemon_init_event_driver("sched_d", "scheduler", sock_addr, use_tcp ? DEFAULT_TCP_PORT : 0,
                                 "scheduler,core", use_tcp, &ev_config, &g_event_driver_sched_d,
                                 &g_bsd_sched_d, &g_bipc_sched_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_sched_d) {
        SVC_LOG_ERROR("Failed to create event driver");

        goto out_server_fd;
    }

    g_dispatcher_sched_d = daemon_event_driver_get_dispatcher(g_event_driver_sched_d);
    method_dispatcher_register(g_dispatcher_sched_d, "register_agent", on_register_agent_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "unregister_agent",
                               on_unregister_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "schedule_task", on_schedule_task_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "get_task", on_get_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "cancel", on_cancel_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_submit", on_dag_submit_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_status", on_dag_status_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_cancel", on_dag_cancel_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "health_check", on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "checkpoint_save", on_checkpoint_save_method,
                               NULL);

    method_dispatcher_register(g_dispatcher_sched_d, "submit", on_schedule_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "query", on_get_task_method, NULL);

    method_dispatcher_register(g_dispatcher_sched_d, "shutdown", on_shutdown_method_sched_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (sched.* namespace)", 13);

    /* Inject the task execution callback and start the queue worker thread:
     * after schedule_task enqueues, the worker asynchronously completes
     * select agent -> spawn -> invoke (real dispatch); get_task queries status */
    sched_service_set_executor(g_service, sched_dispatch_executor);
    if (sched_service_start_workers(g_service) != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start scheduler worker thread");
        goto out_event_driver;
    }

    if (daemon_event_driver_add_server_fd(g_event_driver_sched_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");

        goto out_event_driver;
    }

    SVC_LOG_INFO("Scheduler service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_sched_d);

    daemon_cleanup_standard(g_bipc_sched_d, g_bsd_sched_d, g_event_driver_sched_d, server_fd,
                            destroy_service, &g_running_lock_sched_d);

    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;

out_event_driver:
    daemon_event_driver_destroy(g_event_driver_sched_d);
out_server_fd:
    airy_sock_close(server_fd);
out_service:
    destroy_service();
out_mtx_sock:
    airy_mtx_destroy(&g_running_lock_sched_d);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
