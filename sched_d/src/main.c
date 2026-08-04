#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief 调度服务守护进程主入口（遵循 daemon 模块统一规范）
 *
 * 规范遵循:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 资源确定性(成对管理)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 跨平台一致性(platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 命名语义化(SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 错误可追溯(AIRY_ERR_*)
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

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("sched.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_sched"
#define DEFAULT_TCP_PORT 8083
#define MAX_BUFFER 65536

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(sched_d, scheduler, DEFAULT_SOCKET_PATH_UNIX,
                      DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* ==================== 全局状态 ==================== */

static sched_service_t *g_service = NULL;

/* ==================== 错误码定义（统一使用 AIRY_ERR_*） ==================== */
#define SCHED_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define SCHED_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define SCHED_ERR_NOT_FOUND AIRY_ERR_NOT_FOUND
#define SCHED_ERR_INVALID_CONFIG (AIRY_ERR_DAEMON_BASE + 0x01)
#define SCHED_ERR_STRATEGY_FAIL (AIRY_ERR_DAEMON_BASE + 0x02)

/* ==================== 方法处理器包装函数 ==================== */

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_schedule_task(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_stats(int id, airy_sock_t client_fd);
static void handle_health_check(int id, airy_sock_t client_fd);

/* P2.2 选后派发：声明（实现位于 handle_health_check 之后） */
typedef struct {
    char *agent_id; /* agent_d 派生出的真实 agent_id */
    char *output;   /* agent_d invoke 返回的真实执行输出 */
} sched_dispatch_result_t;
static int sched_dispatch_enabled(void);
static int sched_dispatch_task(const char *role, const char *task_description,
                               sched_dispatch_result_t *out_result);

static void on_register_agent_method(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_schedule_task_method(cJSON *params, int id, void *user_data)
{
    handle_schedule_task(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
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
    /* agent_id/agent_name 为堆指针字段：须 AIRY_STRDUP 分配。
     * 原 AIRY_STRNCPY_TERM(…, sizeof(char*)) 会向 NULL 指针写入 7 字节，
     * 必然 SEGV（既有 P0 缺陷，本处一并修复）。 */
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

static void handle_schedule_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_json = jsonrpc_get_object_param(params, "task");
    if (!task_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task object", id);
        return;
    }

    task_info_t task = {0};
    const char *tid = get_string_field(task_json, "task_id", NULL);
    if (!tid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }
    /* task_id/task_description 为堆指针字段：须 AIRY_STRDUP 分配（同 register_agent
     * 既有 P0 修复：原 AIRY_STRNCPY_TERM(…, sizeof(char*)) 向 NULL 写入导致 SEGV）。 */
    task.task_id = AIRY_STRDUP(tid);
    const char *desc = get_string_field(task_json, "task_description", NULL);
    task.task_description = AIRY_STRDUP(desc ? desc : "");
    if (!task.task_id || !task.task_description) {
        AIRY_FREE(task.task_id);
        AIRY_FREE(task.task_description);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    task.priority = get_int_field(task_json, "priority", 0);
    task.timeout_ms = get_int_field(task_json, "timeout_ms", 30000);

    sched_result_t *result = NULL;
    int ret = sched_service_schedule_task(g_service, &task, &result);

    if (ret != AIRY_SUCCESS || !result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Schedule failed", id);
        SVC_LOG_ERROR("Task scheduling failed: %s (error=%d)", task.task_id, ret);
        AIRY_FREE(task.task_id);
        AIRY_FREE(task.task_description);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "selected_agent_id", result->selected_agent_id);
    cJSON_AddNumberToObject(res_obj, "confidence", result->confidence);
    cJSON_AddNumberToObject(res_obj, "estimated_time_ms", result->estimated_time_ms);

    /* P2.2 选后派发：调度选定 agent 后经 agent_d 真实 spawn+invoke 执行任务。
     * 派发失败如实返回错误（禁止假数据替代真实执行）。 */
    if (result->selected_agent_id && result->selected_agent_id[0] &&
        sched_dispatch_enabled()) {
        sched_dispatch_result_t dispatch = {0};
        int dret = sched_dispatch_task(result->selected_agent_id,
                                       task.task_description, &dispatch);
        if (dret != AIRY_SUCCESS || !dispatch.output || !dispatch.agent_id) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                               "Schedule succeeded but dispatch failed", id);
            SVC_LOG_ERROR("Task dispatch failed: %s -> %s (error=%d)", task.task_id,
                          result->selected_agent_id, dret);
            AIRY_FREE(dispatch.agent_id);
            AIRY_FREE(dispatch.output);
            AIRY_FREE(result->selected_agent_id);
            AIRY_FREE(result);
            AIRY_FREE(task.task_id);
            AIRY_FREE(task.task_description);
            return;
        }
        cJSON_AddBoolToObject(res_obj, "dispatched", true);
        cJSON_AddStringToObject(res_obj, "agent_id", dispatch.agent_id);
        cJSON_AddStringToObject(res_obj, "output", dispatch.output);
        SVC_LOG_INFO("Task dispatched: %s -> Agent %s (role=%s)", task.task_id,
                     dispatch.agent_id, result->selected_agent_id);
        AIRY_FREE(dispatch.agent_id);
        AIRY_FREE(dispatch.output);
    }

    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("Task scheduled: %s -> Agent: %s (Confidence: %.2f)", task.task_id,
                 result->selected_agent_id, result->confidence);

    AIRY_FREE(result->selected_agent_id);
    AIRY_FREE(result);
    AIRY_FREE(task.task_id);
    AIRY_FREE(task.task_description);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    void *stats_data = NULL;
    int ret = sched_service_get_stats(g_service, &stats_data);

    if (ret != AIRY_SUCCESS || !stats_data) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Get stats failed", id);
        return;
    }

    /* P0.18.2: 模式 B — parse + 立即释放 text + 自动释放（JSONRPC_SEND_SUCCESS 内部 Delete） */
    CJSON_PARSE_GUARD(report_json, (char *)stats_data, {
        AIRY_FREE(stats_data);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid report data", id);
        return;
    });
    AIRY_FREE(stats_data);

    JSONRPC_SEND_SUCCESS(client_fd, report_json, id);
    report_json = NULL; /* JSONRPC_SEND_SUCCESS 已 Delete，防止 CJSON_AUTO_FREE 重复释放 */
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

/* ==================== 选后派发（P2.2） ==================== */

/* 是否启用派发：默认开启；AIRY_SCHED_DISPATCH=0 关闭（回归纯调度模式） */
static int sched_dispatch_enabled(void)
{
    const char *env = getenv("AIRY_SCHED_DISPATCH");
    return !(env && env[0] != '\0' && strcmp(env, "0") == 0);
}

/* agent_d Unix socket 路径：AIRY_SCHED_AGENT_SOCK > $AIRY_HOME/run/agent.sock */
static void sched_dispatch_agent_socket(char *buf, size_t size)
{
    const char *env = getenv("AIRY_SCHED_AGENT_SOCK");
    if (env && env[0] != '\0') {
        snprintf(buf, size, "%s", env);
    } else {
        snprintf(buf, size, "%s/agent.sock", airy_runtime_dir());
    }
}

/* 派发单次 RPC 超时（毫秒）：AIRY_SCHED_DISPATCH_TIMEOUT_MS，默认 300000（覆盖真实 LLM 调用） */
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
 * P2.2 真实派发：调度选定角色后，经 agent_d 的 Unix socket 调用
 * agent.spawn + agent.invoke，让该角色对应的真实 Agent 子进程
 * （Python runner → LLM）执行任务，并返回真实输出。
 * 失败时 *out_result 不置位并返回非 0 错误码——调用方必须如实上报，
 * 禁止以假数据替代真实执行（参照 gateway syscall_router 调用 agent.sock 的模式）。
 */
static int sched_dispatch_task(const char *role, const char *task_description,
                               sched_dispatch_result_t *out_result)
{
    if (!role || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    char sock[AIRY_PATH_MAX];
    sched_dispatch_agent_socket(sock, sizeof(sock));

    /* 1. agent.spawn：spec={"role":...,"language":"python"} */
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

    /* 2. agent.invoke：input=task_description（含接力上游产出的任务内容） */
    cJSON *invoke_params = cJSON_CreateObject();
    cJSON_AddStringToObject(invoke_params, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke_params, "input",
                            task_description ? task_description : "");
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
        SVC_LOG_ERROR("sched dispatch: agent.invoke failed (agent=%s, rc=%d)",
                      agent_id, rc);
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

    /* 3. agent.terminate（best-effort：回收子进程资源，失败仅告警不阻断） */
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
                SVC_LOG_WARN("sched dispatch: agent.terminate failed (agent=%s, rc=%d)",
                             agent_id, trc);
        }
    }

    out_result->agent_id = agent_id;
    out_result->output = output;
    SVC_LOG_INFO("sched dispatch: role=%s agent=%s dispatched (output_len=%zu)",
                 role, agent_id, strlen(output));
    return AIRY_SUCCESS;
}

/* ==================== 销毁服务 ==================== */

static void destroy_service(void)
{
    if (g_service) {
        sched_service_destroy(g_service);
        g_service = NULL;
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = "agentrt/manager/service/sched_d/sched.yaml";
    int use_tcp = 0;

    /* 解析命令行参数（--manager/--tcp/--help） */
    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_sched_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    /* 初始化平台层 */
    airy_sock_init();
    airy_mtx_init(&g_running_lock_sched_d);

    /* 设置信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_sched_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(sched_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger）*/
    daemon_cupolas_init("sched_d");

    SVC_LOG_INFO("Scheduler service starting, manager=%s", config_path);

    /* 创建配置 */
    sched_config_t config = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                             .health_check_interval_ms = 5000,
                             .stats_report_interval_ms = 10000,
                             .enable_ml_strategy = false,
                             .ml_model_path = NULL,
                             .max_agents = 100};

    /* 创建调度服务 */
    int ret = sched_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create scheduler service (error=%d)", ret);
        /* N5 修复：改用 goto 集中出口，避免重复清理代码 */
        goto out_mtx_sock;
    }

    SVC_LOG_INFO("Scheduler service created with strategy: round_robin");

    /* 创建服务器 Socket（TCP/Unix/NamedPipe 统一封装） */
    airy_sock_t server_fd = daemon_create_server_socket(
        use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        /* N5 修复：server_fd 未创建，跳过 airy_sock_close，仅清理 service + mtx + sock */
        goto out_service;
    }
    SVC_LOG_INFO(use_tcp ? "Listening on TCP port %d" : "Listening on Unix socket",
                 DEFAULT_TCP_PORT);

    /* 创建事件驱动 + SD/IPC bootstrap */
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
    ret = daemon_init_event_driver("sched_d", "scheduler", sock_addr,
                                   use_tcp ? DEFAULT_TCP_PORT : 0, "scheduler,core", use_tcp,
                                   &ev_config, &g_event_driver_sched_d, &g_bsd_sched_d,
                                   &g_bipc_sched_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_sched_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        /* N5 修复：event_driver 未创建，跳过 destroy，清理 server_fd + service + mtx + sock */
        goto out_server_fd;
    }

    g_dispatcher_sched_d = daemon_event_driver_get_dispatcher(g_event_driver_sched_d);
    method_dispatcher_register(g_dispatcher_sched_d, "register_agent", on_register_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "schedule_task", on_schedule_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "health_check", on_health_check_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods", 4);

    if (daemon_event_driver_add_server_fd(g_event_driver_sched_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        /* N5 修复：event_driver 已创建但 add_server_fd 失败，需 destroy + 清理全部资源 */
        goto out_event_driver;
    }

    SVC_LOG_INFO("Scheduler service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_sched_d);

    /* 标准资源清理链 */
    daemon_cleanup_standard(g_bipc_sched_d, g_bsd_sched_d, g_event_driver_sched_d,
                           server_fd, destroy_service, &g_running_lock_sched_d);

    daemon_cupolas_cleanup(); /* P3.14 ACC-DT15: 清理 cupolas 安全穹顶 */
    log_cleanup();
    return 0;

/* N5 修复：goto 集中出口标签（按资源分配逆序释放，fall-through 模式） */
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
