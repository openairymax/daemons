// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_rpc_handlers.c
 * @brief Scheduler daemon - JSON-RPC method handlers domain.
 * @details Implements the sched.* RPC method handlers (agent register/
 *          unregister, task schedule/get/cancel, DAG submit/status/list/
 *          cancel, stats/health/checkpoint). The on_*_method entry points were
 *          promoted from static when main.c was split by functional domain
 *          (main.c registers them into the method dispatcher; declarations
 *          in sched_daemon_internal.h); the handle_* helpers remain static
 *          here. The daemon bootstrap (main), the service handle ownership
 *          and the agent_d dispatch chain live in main.c / sched_dispatch.c.
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "param_validator.h"
#include "platform.h"
#include "scheduler_service.h"
#include "sched_daemon_internal.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_unregister_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_schedule_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_cancel_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_submit(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_status(cJSON *params, int id, airy_sock_t client_fd);
static void handle_dag_list(int id, airy_sock_t client_fd);
static void handle_dag_cancel(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_stats(int id, airy_sock_t client_fd);
static void handle_health_check(int id, airy_sock_t client_fd);
static void handle_checkpoint_save(cJSON *params, int id, airy_sock_t client_fd);

void on_register_agent_method(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

void on_unregister_agent_method(cJSON *params, int id, void *user_data)
{
    handle_unregister_agent(params, id, *(airy_sock_t *)user_data);
}

void on_schedule_task_method(cJSON *params, int id, void *user_data)
{
    handle_schedule_task(params, id, *(airy_sock_t *)user_data);
}

void on_get_task_method(cJSON *params, int id, void *user_data)
{
    handle_get_task(params, id, *(airy_sock_t *)user_data);
}

void on_cancel_task_method(cJSON *params, int id, void *user_data)
{
    handle_cancel_task(params, id, *(airy_sock_t *)user_data);
}

void on_dag_submit_method(cJSON *params, int id, void *user_data)
{
    handle_dag_submit(params, id, *(airy_sock_t *)user_data);
}

void on_dag_status_method(cJSON *params, int id, void *user_data)
{
    handle_dag_status(params, id, *(airy_sock_t *)user_data);
}

void on_dag_list_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_dag_list(id, *(airy_sock_t *)user_data);
}

void on_dag_cancel_method(cJSON *params, int id, void *user_data)
{
    handle_dag_cancel(params, id, *(airy_sock_t *)user_data);
}

void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

void on_checkpoint_save_method(cJSON *params, int id, void *user_data)
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

    sched_task_info_t task = {0};

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

static void handle_dag_list(int id, airy_sock_t client_fd)
{
    char *json_out = NULL;
    int ret = sched_dag_list_json(g_service, &json_out);
    if (ret != AIRY_SUCCESS || !json_out) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "List DAGs failed", id);
        return;
    }

    CJSON_PARSE_GUARD(report_json, json_out, {
        AIRY_FREE(json_out);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid DAG list data", id);
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
