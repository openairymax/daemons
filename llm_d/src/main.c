#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief LLM 服务守护进程主入口（遵循 daemon 模块统一规范）
 *
 * 规范遵循:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 资源确定性(成对管理)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 跨平台一致性(platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 命名语义化(SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 错误可追溯(AIRY_ERR_*)
 */

/* P0.18.1: 引入 daemon_main.h 提供 DAEMON_DECLARE_COMMON/DAEMON_SETUP_SIGNALS/
 * daemon_parse_args/daemon_create_server_socket/daemon_init_event_driver/daemon_cleanup_standard
 * 等样板宏与内联辅助函数。daemon_main.h 已传递性包含 atomic_compat.h、daemon_bootstrap_*.h、
 * daemon_cupolas_bootstrap.h、daemon_event_driver.h、daemon_platform_ext.h、jsonrpc_helpers.h、
 * logging.h、method_dispatcher.h、svc_logger.h、cjson/cJSON.h、cjson_helpers.h 等头文件，
 * 故此处仅保留业务逻辑直接依赖的头文件。 */
#include "llm_service.h"
#include "response.h"
#include "daemon_main.h"
#include "platform.h"

#include <stdlib.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("llm.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_llm"
#define DEFAULT_TCP_PORT 8080
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define MAX_THREADS 8
#define MAX_MESSAGES_PER_REQUEST 128

/* P0.18.1: 生成公共全局变量（g_running_llm_d 等）、信号处理（signal_handler_llm_d、
 * svc_log_toggle_handler_llm_d）、print_usage_llm_d、daemon_handle_client_llm_d、
 * daemon_on_client_llm_d 等样板，消除手工重复代码。 */
DAEMON_DECLARE_COMMON(llm_d, llm, DEFAULT_SOCKET_PATH_UNIX,
                      DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* ==================== 全局状态 ==================== */

static llm_service_t *g_service = NULL;

/* 服务配置 */
typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_threads;
    int max_clients;
} llm_daemon_config_t;

static llm_daemon_config_t g_config = {0};

/* ==================== 请求上下文（线程安全） ==================== */

typedef struct {
    llm_message_t messages[MAX_MESSAGES_PER_REQUEST];
    size_t message_count;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
} request_context_t;

/**
 * @brief 创建请求上下文
 */
static request_context_t *request_context_create(void)
{
    request_context_t *ctx = (request_context_t *)AIRY_CALLOC(1, sizeof(request_context_t));
    if (!ctx) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    ctx->response_capacity = MAX_BUFFER;
    ctx->response_buffer = (char *)AIRY_MALLOC(ctx->response_capacity);
    if (!ctx->response_buffer) {
        AIRY_FREE(ctx);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    ctx->response_buffer[0] = '\0';
    ctx->response_size = 0;

    return ctx;
}

/**
 * @brief 销毁请求上下文
 */
static void request_context_destroy(request_context_t *ctx)
{
    if (!ctx)
        return;

    for (size_t i = 0; i < ctx->message_count; i++) {
        AIRY_FREE((void *)ctx->messages[i].role);
        AIRY_FREE((void *)ctx->messages[i].content);
    }

    AIRY_FREE(ctx->response_buffer);
    AIRY_FREE(ctx);
}

/* ==================== 参数解析（线程安全） ==================== */

/**
 * @brief 解析请求参数为 llm_request_config_t
 * @param params JSON 参数对象
 * @param ctx 请求上下文（用于存储消息）
 * @param cfg 输出配置
 * @return 0 成功，非0 失败
 */
static void parse_params_cleanup(request_context_t *ctx, llm_request_config_t *cfg)
{
    if (cfg->model) {
        AIRY_FREE((void *)cfg->model);
        cfg->model = NULL;
    }
    for (size_t i = 0; i < ctx->message_count; i++) {
        AIRY_FREE((void *)ctx->messages[i].role);
        AIRY_FREE((void *)ctx->messages[i].content);
    }
    ctx->message_count = 0;
}

static int parse_params(cJSON *params, request_context_t *ctx, llm_request_config_t *cfg)
{
    __builtin_memset(cfg, 0, sizeof(llm_request_config_t));

    cJSON *model = cJSON_GetObjectItem(params, "model");
    if (!cJSON_IsString(model)) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "model parameter is not a string");
    }
    cfg->model = AIRY_STRDUP(model->valuestring);
    if (!cfg->model) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate model string");
    }

    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    if (cJSON_IsArray(messages)) {
        size_t count = cJSON_GetArraySize(messages);
        if (count > MAX_MESSAGES_PER_REQUEST) {
            parse_params_cleanup(ctx, cfg);
            AIRY_ERROR(AIRY_ERR_OVERFLOW, "too many messages");
        }

        ctx->message_count = count;
        cfg->message_count = count;
        cfg->messages = ctx->messages;

        for (size_t i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(messages, i);
            cJSON *role = cJSON_GetObjectItem(item, "role");
            cJSON *content = cJSON_GetObjectItem(item, "content");

            if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "message role or content is not a string");
            }

            ctx->messages[i].role = AIRY_STRDUP(role->valuestring);
            ctx->messages[i].content = AIRY_STRDUP(content->valuestring);

            if (!ctx->messages[i].role || !ctx->messages[i].content) {
                ctx->message_count = i;
                parse_params_cleanup(ctx, cfg);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate message role or content");
            }
        }
    }

    /* 解析可选参数 */
    cJSON *temp = cJSON_GetObjectItem(params, "temperature");
    if (cJSON_IsNumber(temp)) {
        cfg->temperature = (float)temp->valuedouble;
    }

    cJSON *top_p = cJSON_GetObjectItem(params, "top_p");
    if (cJSON_IsNumber(top_p)) {
        cfg->top_p = (float)top_p->valuedouble;
    }

    cJSON *max_tokens = cJSON_GetObjectItem(params, "max_tokens");
    if (cJSON_IsNumber(max_tokens)) {
        cfg->max_tokens = max_tokens->valueint;
    }

    cJSON *stream = cJSON_GetObjectItem(params, "stream");
    if (cJSON_IsBool(stream)) {
        cfg->stream = cJSON_IsTrue(stream) ? 1 : 0;
    }

    cJSON *presence_penalty = cJSON_GetObjectItem(params, "presence_penalty");
    if (cJSON_IsNumber(presence_penalty)) {
        cfg->presence_penalty = presence_penalty->valuedouble;
    }

    cJSON *frequency_penalty = cJSON_GetObjectItem(params, "frequency_penalty");
    if (cJSON_IsNumber(frequency_penalty)) {
        cfg->frequency_penalty = frequency_penalty->valuedouble;
    }

    return 0;
}

/* ==================== 方法处理器包装函数 ==================== */

/* 前向声明 */
static char *handle_complete(cJSON *params, int id);
static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd);

/**
 * @brief complete 方法的包装器（适配 method_dispatcher 接口）
 */
static void on_complete_method(cJSON *params, int id, void *user_data __attribute__((unused)))
{
    char *response = handle_complete(params, id);
    if (response) {
        airy_sock_t client_fd = *(airy_sock_t *)user_data;
        size_t resp_len = strlen(response);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send start fd=%d resp_len=%zu", (int)client_fd, resp_len);
        ssize_t sent = airy_sock_send(client_fd, response, resp_len);
        if (getenv("AIRY_LLM_D_DIAG"))
            SVC_LOG_ERROR("llm_d diag: complete send done fd=%d sent=%zd errno=%d", (int)client_fd, sent, errno);
        AIRY_FREE(response);
    }
}

/**
 * @brief complete_stream 方法的包装器
 */
static void on_complete_stream_method(cJSON *params, int id,
                                      void *user_data __attribute__((unused)))
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    char *response = handle_complete_stream(params, id, client_fd);
    if (response) {
        airy_sock_send(client_fd, response, strlen(response));
        AIRY_FREE(response);
    }
}

/* ==================== 请求处理 ==================== */

/**
 * @brief 处理 complete 方法
 */
static char *handle_complete(cJSON *params, int id)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    uint64_t start_time = airy_time_ms();

#define LLM_MAX_RETRIES 3
#define LLM_BASE_DELAY_MS 100

    llm_response_t *resp = NULL;
    int ret = -1;

    for (int attempt = 0; attempt <= LLM_MAX_RETRIES; attempt++) {
        ret = llm_service_complete(g_service, &cfg, &resp);

        if (ret == 0)
            break;

        if (attempt < LLM_MAX_RETRIES) {
            unsigned delay_ms = LLM_BASE_DELAY_MS * (1U << (attempt > 15 ? 15 : attempt));
            SVC_LOG_WARN("LLM complete attempt %d/%d failed (err=%d), retrying in %ums",
                         attempt + 1, LLM_MAX_RETRIES + 1, ret, delay_ms);
            airy_sleep_ms(delay_ms);
        }
    }

    uint64_t end_time = airy_time_ms();

    if (ret != 0) {
        SVC_LOG_ERROR("LLM complete failed after %d attempts (total %llums)", LLM_MAX_RETRIES + 1,
                      (unsigned long long)(end_time - start_time));
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "LLM service unavailable after retries",
                                   id);
    }

    char *resp_json = response_to_json(resp);
    llm_response_free(resp);
    AIRY_FREE((void *)cfg.model);

    if (!resp_json) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Failed to serialize response", id);
    }

    /* P0.18.2: 模式 B — CJSON_PARSE_GUARD（on_fail 中释放 resp_json） */
    CJSON_PARSE_GUARD(result, resp_json, {
        AIRY_FREE(resp_json);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Invalid response format", id);
    });
    AIRY_FREE(resp_json);

    char *success = jsonrpc_build_success(result, id);
    /* P0.20.8 修复：jsonrpc_build_success 通过 cJSON_AddItemToObject 转移 result 所有权到
     * root，cJSON_Delete(root) 递归释放了 result。必须置 NULL 防止 CJSON_AUTO_FREE
     * 清理时 cJSON_Delete(result) double-free。 */
    result = NULL;

    request_context_destroy(ctx);
    return success;
}

/**
 * @brief 处理 complete_stream 方法
 */
typedef struct {
    airy_sock_t fd;
} llm_stream_ctx_t;

static void llm_stream_callback(const char *chunk, void *user_data)
{
    llm_stream_ctx_t *sctx = (llm_stream_ctx_t *)user_data;
    airy_sock_send(sctx->fd, chunk, strlen(chunk));
}

static char *handle_complete_stream(cJSON *params, int id, airy_sock_t client_fd)
{
    request_context_t *ctx = request_context_create();
    if (!ctx) {
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }

    llm_request_config_t cfg;
    if (parse_params(params, ctx, &cfg) != 0) {
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Invalid params", id);
    }

    cfg.stream = 1;

    llm_stream_ctx_t stream_ctx = {.fd = client_fd};

    llm_response_t *resp = NULL;
    int ret = llm_service_complete_stream(g_service, &cfg, llm_stream_callback, &stream_ctx, &resp);

    if (ret != 0) {
        AIRY_FREE((void *)cfg.model);
        request_context_destroy(ctx);
        return jsonrpc_build_error(JSONRPC_INTERNAL_ERROR, "Service error", id);
    }

    if (resp) {
        llm_response_free(resp);
    }

    AIRY_FREE((void *)cfg.model);
    request_context_destroy(ctx);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/* ==================== 配置加载 ==================== */

/**
 * @brief 加载守护进程配置
 */
static int load_daemon_config(const char *config_path)
{
    /* 默认配置 */
    g_config.use_tcp = 0;
    g_config.max_threads = MAX_THREADS;
    g_config.max_clients = MAX_CLIENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    /* 如果提供了配置文件，尝试加载 */
    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);

            char *content = (char *)AIRY_MALLOC(len + 1);
            if (content) {
                size_t nread = fread(content, 1, len, f);
                if (nread == (size_t)len) {
                    content[len] = '\0';

                    /* P0.18.2: CJSON_PARSE_GUARD 替代 cJSON_Parse + if (root) + 手动 cJSON_Delete
                     * 使用 do { ... } while (0) + break 保持原 if (root) 块语义：解析失败时跳过配置提取 */
                    do {
                        CJSON_PARSE_GUARD(root, content, { break; });
                        cJSON *daemon = cJSON_GetObjectItem(root, "daemon");
                        if (daemon) {
                            cJSON *socket_path = cJSON_GetObjectItem(daemon, "socket_path");
                            if (cJSON_IsString(socket_path)) {
                                AIRY_FREE(g_config.socket_path);
                                g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                            }

                            cJSON *tcp_port = cJSON_GetObjectItem(daemon, "tcp_port");
                            if (cJSON_IsNumber(tcp_port)) {
                                g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                g_config.use_tcp = 1;
                            }

                            cJSON *max_threads = cJSON_GetObjectItem(daemon, "max_threads");
                            if (cJSON_IsNumber(max_threads)) {
                                g_config.max_threads = max_threads->valueint;
                            }
                        }
                        /* root 由 CJSON_AUTO_FREE 自动释放（do-while 作用域退出时） */
                    } while (0);
                }
                AIRY_FREE(content);
            }
            fclose(f);
        }
    }

    return 0;
}

/**
 * @brief 释放配置资源
 */
static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

/* ==================== 销毁服务（daemon_cleanup_standard 回调） ==================== */

static void destroy_service_llm_d(void)
{
    if (g_service) {
        llm_service_destroy(g_service);
        g_service = NULL;
    }
    free_daemon_config();
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    /* P0.18.1: 统一命令行参数解析（--manager/--tcp/--help） */
    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_llm_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    /* 初始化平台层 */
    airy_sock_init();
    airy_mtx_init(&g_running_lock_llm_d);

    /* P0.18.1: 跨平台信号处理设置 */
#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_llm_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(llm_d);
#endif

    /* 保留初始日志级别 WARN（SIGUSR1 切换在 DEBUG/INFO 间切换，详见生成的 svc_log_toggle_handler_llm_d）
     * 调试：AIRY_LLM_D_DEBUG=1 时输出 DEBUG 级日志 */
    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_LLM_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1')
                        ? (airy_log_level_t)LOG_LEVEL_DEBUG
                        : (airy_log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger）*/
    daemon_cupolas_init("llm_d");

    /* 加载配置（配置文件中的 tcp_port 会置位 g_config.use_tcp） */
    load_daemon_config(config_path);
    use_tcp = use_tcp || g_config.use_tcp;

    SVC_LOG_INFO("LLM service starting, manager=%s", config_path ? config_path : "default");

    /* 创建 LLM 服务 */
    g_service = llm_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create service");
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* P0.18.1: 统一服务器 Socket 创建（TCP/Unix/NamedPipe 封装） */
    int tcp_port = g_config.tcp_port ? (int)g_config.tcp_port : DEFAULT_TCP_PORT;
    const char *unix_path = g_config.socket_path ? g_config.socket_path : DEFAULT_SOCKET_PATH_UNIX;
    airy_sock_t server_fd = daemon_create_server_socket(use_tcp, tcp_port, unix_path,
                                                              DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    if (use_tcp)
        SVC_LOG_INFO("Listening on TCP port %d", tcp_port);
    else
        SVC_LOG_INFO("Listening on %s", unix_path);

    /* P0.18.1: 创建事件驱动 + SD/IPC bootstrap（统一封装） */
    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = g_config.max_threads > 0 ? g_config.max_threads : 4;
    ev_config.thread_pool_max = g_config.max_threads > 0 ? g_config.max_threads : 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_llm_d; /* P0.18.1: 使用生成的客户端处理回调 */
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : unix_path;
    int ret = daemon_init_event_driver("llm_d", "llm", sock_addr,
                                       use_tcp ? tcp_port : 0, "ai,core", use_tcp,
                                       &ev_config, &g_event_driver_llm_d, &g_bsd_llm_d,
                                       &g_bipc_llm_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_llm_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_llm_d = daemon_event_driver_get_dispatcher(g_event_driver_llm_d);
    method_dispatcher_register(g_dispatcher_llm_d, "complete", on_complete_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "complete_stream", on_complete_stream_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods", 2);

    if (daemon_event_driver_add_server_fd(g_event_driver_llm_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_llm_d);
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("LLM service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_llm_d);

    /* P0.18.1: 标准资源清理链（与 init 相反顺序） */
    daemon_cleanup_standard(g_bipc_llm_d, g_bsd_llm_d, g_event_driver_llm_d,
                           server_fd, destroy_service_llm_d, &g_running_lock_llm_d);

    daemon_cupolas_cleanup(); /* P3.14 ACC-DT15: 清理 cupolas 安全穹顶 */
    log_cleanup();
    return 0;
}
