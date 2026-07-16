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
#include "param_validator.h"
#include "scheduler_service.h"
#include "strategy_interface.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <time.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX AIRY_RUNTIME_DIR "/sched.sock"
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

AIRY_STRNCPY_TERM(info.agent_id, aid, sizeof(info.agent_id));
    (info.agent_id)[sizeof(info.agent_id) - 1] = '\0';
    const char *aname = get_string_field(agent_json, "agent_name", NULL);
    if (aname)
AIRY_STRNCPY_TERM(info.agent_name, aname, sizeof(info.agent_name));
        (info.agent_name)[sizeof(info.agent_name) - 1] = '\0';

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

AIRY_STRNCPY_TERM(task.task_id, tid, sizeof(task.task_id));
    (task.task_id)[sizeof(task.task_id) - 1] = '\0';

    const char *desc = get_string_field(task_json, "task_description", NULL);
    if (desc)
AIRY_STRNCPY_TERM(task.task_description, desc, sizeof(task.task_description));
        (task.task_description)[sizeof(task.task_description) - 1] = '\0';

    task.priority = get_int_field(task_json, "priority", 0);
    task.timeout_ms = get_int_field(task_json, "timeout_ms", 30000);

    sched_result_t *result = NULL;
    int ret = sched_service_schedule_task(g_service, &task, &result);

    if (ret != AIRY_SUCCESS || !result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Schedule failed", id);
        SVC_LOG_ERROR("Task scheduling failed: %s (error=%d)", task.task_id, ret);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "selected_agent_id", result->selected_agent_id);
    cJSON_AddNumberToObject(res_obj, "confidence", result->confidence);
    cJSON_AddNumberToObject(res_obj, "estimated_time_ms", result->estimated_time_ms);

    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
    SVC_LOG_INFO("Task scheduled: %s -> Agent: %s (Confidence: %.2f)", task.task_id,
                 result->selected_agent_id, result->confidence);

    AIRY_FREE(result->selected_agent_id);
    AIRY_FREE(result);
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
