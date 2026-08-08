// SPDX-FileCopyrightText: 2026 SPHARX.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
// @owner: team-B
/**
 * @file gateway_business_handler.c
 * @brief 网关业务请求处理器实现（agent.run → llm_d 转发）
 *
 * SEC-017 合规：所有功能均为真实实现，无桩函数。
 * 处理链：HTTP JSON-RPC agent.run → 直连 llm_d(complete) → 返回对话结果。
 */

#include "gateway_business_handler.h"

#include "airy_memory.h"
#include "atomic_compat.h"
#include "logging.h"
#include "platform.h"
#include "gateway_protocol_router.h"
#include "daemon_security.h"
/* A2-1: model.yaml global 段提取公共 API（与 llm_d 同一实现） */
#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ==================== 常量 ==================== */

#define GW_LLM_DEFAULT_MODEL "deepseek-v4-flash" /* 与 model.yaml global.default_model 对齐 */
#define GW_LLM_DEFAULT_TIMEOUT_MS 30000
#define GW_LLM_MAX_RESP 1048576 /* 1MB 响应上限 */
#define GW_LLM_DEFAULT_TCP_PORT 8080
#define GW_SESSION_ID_LEN 64
#define GW_EXTERNAL_AGENT_ID "external" /* 外部协议请求默认 ACL 身份（R4） */

/* ==================== 运行中请求注册表（支持人工中止 agent.cancel） ==================== */

/**
 * @brief 运行中 agent.run 请求条目
 *
 * agent.run 是同步阻塞链路（LLM 往返 + 工具循环），单次可达数分钟。
 * 为支持「任务开始后人工中止」，网关维护运行中请求注册表：
 *   - handle_agent_run 注册条目（session_id → cancelled=0），结束注销；
 *   - agent.cancel(session_id) 置位 cancelled，工具循环轮次间检查并中断。
 */
typedef struct gw_active_request_s {
    char session_id[GW_SESSION_ID_LEN];
    atomic_int cancelled; /* 0=运行中，1=已请求取消 */
    struct gw_active_request_s *next;
} gw_active_request_t;

/* ==================== 上下文 ==================== */

struct gateway_business_ctx_s {
    char llm_sock_path[256]; /* POSIX: llm_d Unix socket 路径 */
    char llm_tcp_addr[64];   /* Windows: llm_d TCP 地址 */
    uint16_t llm_tcp_port;   /* Windows: llm_d TCP 端口 */
    char tool_sock_path[256]; /* POSIX: tool_d Unix socket 路径（工具执行） */
    char agent_sock_path[256]; /* POSIX: agent_d Unix socket 路径（agent 编排） */
    char mem_sock_path[256];   /* POSIX: mem_d Unix socket 路径（记忆服务） */
    char sched_sock_path[256]; /* POSIX: sched_d Unix socket 路径（调度服务） */
    char default_model[128]; /* 默认模型（env AIRY_AGENT_MODEL 或内置默认） */
    /* 运行中请求注册表（agent.cancel 支持） */
    airy_mtx_t active_lock;
    gw_active_request_t *active_requests;
};

/* ==================== JSON-RPC 错误响应 ==================== */

static char *jsonrpc_error(int code, const char *msg, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "Unknown error");
    cJSON_AddItemToObject(resp, "error", err);

    if (id && !cJSON_IsNull(id)) {
        if (cJSON_IsString(id)) {
            cJSON_AddStringToObject(resp, "id", id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(resp, "id");
        }
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

/* ==================== llm_d 连接与调用 ==================== */

#ifdef _WIN32
static int llm_connect_tcp(const gateway_business_ctx_t *ctx)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -1;
    struct sockaddr_in addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->llm_tcp_port);
    inet_pton(AF_INET, ctx->llm_tcp_addr, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }
    return (int)fd;
}
#else
static int llm_connect_unix(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/**
 * @brief 向 llm_d 发送 JSON-RPC complete 请求并读取响应
 * @return 响应字符串（AIRY_MALLOC），失败返回 NULL
 */
static char *llm_call_complete(const gateway_business_ctx_t *ctx, const char *req_json)
{
    int fd;
#ifdef _WIN32
    fd = llm_connect_tcp(ctx);
#else
    fd = llm_connect_unix(ctx->llm_sock_path);
#endif
    if (fd < 0) {
        LOG_WARN("gateway handler: cannot connect to llm_d (sock=%s)", ctx->llm_sock_path);
        return NULL;
    }

    /* 设置接收超时，避免 llm_d 异常时阻塞请求线程 */
#ifdef _WIN32
    int timeout_ms = GW_LLM_DEFAULT_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = {GW_LLM_DEFAULT_TIMEOUT_MS / 1000,
                         (GW_LLM_DEFAULT_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    size_t len = strlen(req_json);
    size_t sent_total = 0;
    while (sent_total < len) {
#ifdef _WIN32
        int n = send(fd, req_json + sent_total, (int)(len - sent_total), 0);
#else
        ssize_t n = send(fd, req_json + sent_total, len - sent_total, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            return NULL;
        }
        sent_total += (size_t)n;
    }

    /* 读取响应（阻塞直到连接关闭或超时） */
    size_t cap = 4096;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return NULL;
    }
    resp[0] = '\0';

    char buf[4096];
    for (;;) {
#ifdef _WIN32
        int n = recv(fd, buf, sizeof(buf), 0);
#else
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return resp;
}

/* ==================== tool_d 客户端（工具执行） ==================== */

/* 内置工具 OpenAI tools schema（与 tool_d builtin.c 注册的工具一一对应） */
static const char GW_TOOLS_JSON[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_read\","
    "\"description\":\"Read a file's content from the local filesystem\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"fs_write\","
    "\"description\":\"Write content to a local file (creates or overwrites)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"fs_list\","
    "\"description\":\"List entries of a local directory (JSON array)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"shell_run\","
    "\"description\":\"Execute a shell command and capture its output\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}}"
    "]";

#define GW_MAX_TOOL_LOOPS 8 /* 单次 agent.run 工具循环上限（防失控） */
#define GW_TOOL_TIMEOUT_MS 30000

/* agent 编排超时：spawn 含 Python runner 冷启动（秒级），invoke 含 LLM 往返（可达数十秒） */
#define GW_AGENT_SPAWN_TIMEOUT_MS 90000
#define GW_AGENT_INVOKE_TIMEOUT_MS 180000

/**
 * @brief 向 tool_d 发送 JSON-RPC 请求并读取响应（POSIX Unix socket）
 * @return 响应字符串（AIRY_MALLOC），失败返回 NULL
 */
static char *tool_call_rpc(const gateway_business_ctx_t *ctx, const char *req_json)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, ctx->tool_sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("gateway handler: cannot connect to tool_d (sock=%s)", ctx->tool_sock_path);
        close(fd);
        return NULL;
    }

    struct timeval tv = {GW_TOOL_TIMEOUT_MS / 1000, (GW_TOOL_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    (void)ctx;
    (void)req_json;
    return NULL;
#endif
}

/* ==================== 通用 daemon 内部服务调用（L2 服务协议客户端） ==================== */

/**
 * @brief 通用 daemon 内部服务调用（Unix socket JSON-RPC）
 *
 * 构造 {"jsonrpc":"2.0","method":<method>,"params":<params_json>,"id":1} 发送到
 * 目标 daemon socket 并阻塞读取完整 JSON 响应。gateway 作为 L2 服务协议
 * （<daemon>.<method>）的客户端调用各 daemon，daemon 侧无需感知外部协议。
 *
 * @param sock_path   目标 daemon socket 路径
 * @param method      内部服务方法（如 "spawn"/"invoke"/"write"）
 * @param params_json 方法参数 JSON 字符串（NULL/空 → "{}"）
 * @param timeout_ms  接收超时（毫秒）
 * @return 响应 JSON 字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 */
static char *gw_svc_call(const char *sock_path, const char *method,
                         const char *params_json, int timeout_ms)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        close(fd);
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        close(fd);
        return NULL;
    }

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
        if (n <= 0) {
            AIRY_FREE(req_str);
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    (void)sock_path;
    (void)method;
    (void)params_json;
    (void)timeout_ms;
    return NULL;
#endif
}

/* ==================== 协议层 ACL（R4 fail-closed） ==================== */

/**
 * @brief 外部协议工具执行 ACL 检查
 *
 * fail-closed：daemon_check_tool_permission 对未注册规则（agent_id, tool_name）
 * 一律 DENY。默认规则在 main.c 启动时注册（fs_read/fs_write/fs_list allow，
 * shell_run 按 AIRY_GATEWAY_ACL_ALLOW_SHELL 环境变量，默认 deny）。
 *
 * @param tool_name 工具名
 * @return 0 允许，非 0 拒绝
 */
static int gw_acl_check_tool(const char *tool_name)
{
    if (!tool_name)
        return -1;
    int rc = daemon_check_tool_permission(GW_EXTERNAL_AGENT_ID, tool_name, "execute");
    if (rc != 0) {
        LOG_WARN("gateway ACL DENY: agent=%s tool=%s (fail-closed)", GW_EXTERNAL_AGENT_ID,
                 tool_name);
        return -1;
    }
    return 0;
}

/* ==================== mem.* 转发（G3） ==================== */

/**
 * @brief mem.* 方法白名单：返回 mem_d 内部方法名，非白名单返回 NULL
 *
 * 仅透传标准 5 方法（02-l2-service-protocol.md mem.* 命名空间），
 * 其他 mem.* 方法拒绝——防止任意方法穿透到 mem_d。
 */
static const char *gw_mem_method_allowlist(const char *method)
{
    if (!method)
        return NULL;
    if (strcmp(method, "mem.write") == 0)
        return "write";
    if (strcmp(method, "mem.search") == 0)
        return "search";
    if (strcmp(method, "mem.get") == 0)
        return "get";
    if (strcmp(method, "mem.delete") == 0)
        return "delete";
    if (strcmp(method, "mem.count") == 0)
        return "count";
    return NULL;
}

/**
 * @brief mem.* 转发：gateway JSON-RPC → mem_d（params/响应透传）
 *
 * env 门控 AIRY_GATEWAY_MEM_PUBLIC（默认 true：内部记忆服务正常业务放行；
 * false 关闭外部 mem 访问，不影响 TUI 本地 JSONL）。
 */
static char *handle_mem_call(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *mem_method = gw_mem_method_allowlist(cJSON_IsString(method)
                                                         ? method->valuestring
                                                         : NULL);
    if (!mem_method) {
        return jsonrpc_error(-32601, "Method not found", id);
    }

    const char *pub = getenv("AIRY_GATEWAY_MEM_PUBLIC");
    if (pub && (strcmp(pub, "false") == 0 || strcmp(pub, "0") == 0)) {
        return jsonrpc_error(-32001, "Memory service access disabled", id);
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    char *resp = gw_svc_call(ctx->mem_sock_path, mem_method, params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        return jsonrpc_error(-32603, "Memory service unreachable", id);
    }

    /* 透传 mem_d 完整 JSON-RPC 响应（result/error 原样） */
    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Memory service returned invalid response", id);
    }
    /* JSON-RPC 2.0 合规：响应 id 必须与请求 id 一致。
     * mem_d 回显的是 gw_svc_call 内部 id=1，若不改写，并发请求
     * 无法将响应关联到原请求（客户端校验 id 会失败）。 */
    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *mem_id = cJSON_GetObjectItem(rroot, "id");
    if (mem_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief llm.list_models 转发：gateway JSON-RPC → llm_d list_models
 *
 * 返回 llm_d provider registry 全量模型 + default_model/default_provider，
 * 供 CLI/TUI 模型自由配置使用（只读、无参数、不需要 API key）。
 * 响应 id 改写为请求 id（同 handle_mem_call 的并发合规）。
 */
static char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");

    char *resp = gw_svc_call(ctx->llm_sock_path, "list_models", "{}",
                             GW_LLM_DEFAULT_TIMEOUT_MS);
    if (!resp) {
        return jsonrpc_error(-32603, "LLM service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "LLM service returned invalid response", id);
    }

    /* JSON-RPC 2.0 合规：响应 id 与请求 id 一致 */
    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *llm_id = cJSON_GetObjectItem(rroot, "id");
    if (llm_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/* ==================== agent.run 处理 ==================== */

/* ---- 运行中请求注册表：agent.cancel 支持 ---- */

/**
 * @brief 生成唯一会话 ID（时间 + 自增计数器 + 随机位，避免 time(NULL) 伪会话）
 */
static void gw_gen_session_id(char *out, size_t out_size)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t rand_bits = 0;
    {
        /* 跨平台伪随机（足够生成不可预测会话 ID，非密码学用途） */
        uint64_t *p = (uint64_t *)&now;
        rand_bits = ((*p) ^ (s << 32)) * 6364136223846793005ULL;
    }
    snprintf(out, out_size, "sess_%016llx_%04llx", (unsigned long long)(now ^ rand_bits),
             (unsigned long long)(s & 0xFFFF));
}

/**
 * @brief 注册运行中请求（cancelled=0）
 */
static gw_active_request_t *gw_active_register(gateway_business_ctx_t *ctx,
                                               const char *session_id)
{
    gw_active_request_t *entry =
        (gw_active_request_t *)AIRY_CALLOC(1, sizeof(gw_active_request_t));
    if (!entry)
        return NULL;
    AIRY_STRNCPY_TERM(entry->session_id, session_id, sizeof(entry->session_id));
    atomic_store_explicit(&entry->cancelled, 0, memory_order_relaxed);
    airy_mtx_lock(&ctx->active_lock);
    entry->next = ctx->active_requests;
    ctx->active_requests = entry;
    airy_mtx_unlock(&ctx->active_lock);
    LOG_INFO("gateway: agent.run registered (session=%s)", entry->session_id);
    return entry;
}

/**
 * @brief 注销运行中请求
 */
static void gw_active_unregister(gateway_business_ctx_t *ctx, gw_active_request_t *entry)
{
    if (!entry)
        return;
    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t **pp = (gw_active_request_t **)&ctx->active_requests;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }
    airy_mtx_unlock(&ctx->active_lock);
    LOG_INFO("gateway: agent.run unregistered (session=%s)", entry->session_id);
    AIRY_FREE(entry);
}

/**
 * @brief 是否已请求取消
 */
static bool gw_active_is_cancelled(gw_active_request_t *entry)
{
    return entry && atomic_load_explicit(&entry->cancelled, memory_order_relaxed) != 0;
}

/**
 * @brief Agent 编排路径：spawn + invoke（params.agent → agent_d）
 * 调用 agent_d 的 spawn（agent_spec）→ 获取 agent_id → invoke（input=prompt）
 * → 返回 output。Agent 生命周期由 agent_d 管理（idle 自动回收，
 * 见 agent_service_reap_idle），网关不持有 agent 状态。
 *
 * @param ctx 网关上下文（含 agent_sock_path）
 * @param agent_spec params.agent（JSON 对象，含 role/language 等字段）
 * @param prompt 用户输入（作为 invoke 的 input）
 * @param out_text 最终输出（AIRY_MALLOC，调用者 AIRY_FREE）
 * @param out_err 失败原因（AIRY_MALLOC，调用者 AIRY_FREE；成功为 NULL）
 * @return 0 成功，非 0 失败
 */
static int gw_agent_run_orchestrate(const gateway_business_ctx_t *ctx,
                                    const cJSON *agent_spec,
                                    const char *prompt,
                                    char **out_text, char **out_err)
{
    *out_text = NULL;
    *out_err = NULL;
    if (!cJSON_IsObject(agent_spec)) {
        *out_err = AIRY_STRDUP("params.agent must be a JSON object (role/language/...)");
        return -1;
    }

    char *spec_str = cJSON_PrintUnformatted(agent_spec);
    if (!spec_str) {
        *out_err = AIRY_STRDUP("cannot serialize params.agent");
        return -1;
    }

    /* 1. spawn：{"agent_spec": <spec>}（spec_str 为合法 JSON，可直接内嵌） */
    size_t spawn_n = strlen(spec_str) + 32;
    char *spawn_params = (char *)AIRY_MALLOC(spawn_n);
    if (!spawn_params) {
        AIRY_FREE(spec_str);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    snprintf(spawn_params, spawn_n, "{\"agent_spec\":%s}", spec_str);
    AIRY_FREE(spec_str);

    char *spawn_resp = gw_svc_call(ctx->agent_sock_path, "spawn", spawn_params,
                                   GW_AGENT_SPAWN_TIMEOUT_MS);
    AIRY_FREE(spawn_params);
    if (!spawn_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (spawn)");
        return -1;
    }

    char *agent_id = NULL;
    cJSON *sroot = cJSON_Parse(spawn_resp);
    if (sroot) {
        cJSON *err = cJSON_GetObjectItem(sroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(sroot, "result");
        cJSON *aid = result ? cJSON_GetObjectItem(result, "agent_id") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(aid) && aid->valuestring) {
            agent_id = AIRY_STRDUP(aid->valuestring);
        } else {
            *out_err = AIRY_STRDUP("agent.spawn returned no agent_id");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.spawn returned invalid response");
    }
    cJSON_Delete(sroot);
    AIRY_FREE(spawn_resp);
    if (!agent_id) {
        if (!*out_err)
            *out_err = AIRY_STRDUP("agent.spawn failed");
        return -1;
    }

    /* 2. invoke：{"agent_id": <id>, "input": <prompt>}（cJSON 负责转义） */
    cJSON *invoke_params = cJSON_CreateObject();
    if (!invoke_params) {
        AIRY_FREE(agent_id);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    cJSON_AddStringToObject(invoke_params, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke_params, "input", prompt ? prompt : "");
    char *invoke_params_str = cJSON_PrintUnformatted(invoke_params);
    cJSON_Delete(invoke_params);
    AIRY_FREE(agent_id);
    if (!invoke_params_str) {
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }

    char *invoke_resp = gw_svc_call(ctx->agent_sock_path, "invoke", invoke_params_str,
                                    GW_AGENT_INVOKE_TIMEOUT_MS);
    AIRY_FREE(invoke_params_str);
    if (!invoke_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (invoke)");
        return -1;
    }

    int rc = -1;
    cJSON *iroot = cJSON_Parse(invoke_resp);
    if (iroot) {
        cJSON *err = cJSON_GetObjectItem(iroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(iroot, "result");
        cJSON *out = result ? cJSON_GetObjectItem(result, "output") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(out)) {
            *out_text = AIRY_STRDUP(out->valuestring);
            rc = 0;
        } else {
            *out_err = AIRY_STRDUP("agent.invoke returned no output");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.invoke returned invalid response");
    }
    cJSON_Delete(iroot);
    AIRY_FREE(invoke_resp);
    return rc;
}

/**
 * @brief 从 llm 响应提取对话文本与 token 用量
 * @return 0 成功（*out_text / *out_tokens / *out_cost 有效），非 0 失败
 */
static int parse_llm_result(const char *llm_resp, char **out_text, uint64_t *out_tokens,
                            double *out_cost)
{
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        LOG_WARN("gateway handler: llm_d error: %s", cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 = (choices && cJSON_GetArraySize(choices) > 0)
                         ? cJSON_GetArrayItem(choices, 0)
                         : NULL;
    cJSON *content = choice0 ? cJSON_GetObjectItem(choice0, "content") : NULL;
    if (!cJSON_IsString(content)) {
        cJSON_Delete(root);
        return -1;
    }
    *out_text = AIRY_STRDUP(content->valuestring);

    /* token：优先 usage.total_tokens（OpenAI 兼容），其次顶层 total_tokens（llm_d 旧格式）。
     * 两处缺失时尝试 usage 内 prompt+completion 求和。 */
    cJSON *usage = result ? cJSON_GetObjectItem(result, "usage") : NULL;
    cJSON *total = usage ? cJSON_GetObjectItem(usage, "total_tokens") : NULL;
    if (!cJSON_IsNumber(total)) {
        total = result ? cJSON_GetObjectItem(result, "total_tokens") : NULL;
    }
    if (!cJSON_IsNumber(total) && cJSON_IsObject(usage)) {
        cJSON *u_pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *u_ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (cJSON_IsNumber(u_pt) || cJSON_IsNumber(u_ct)) {
            uint64_t sum = 0;
            if (cJSON_IsNumber(u_pt))
                sum += (uint64_t)(u_pt->valuedouble > 0 ? u_pt->valuedouble : 0);
            if (cJSON_IsNumber(u_ct))
                sum += (uint64_t)(u_ct->valuedouble > 0 ? u_ct->valuedouble : 0);
            *out_tokens = sum;
        }
    } else if (cJSON_IsNumber(total)) {
        *out_tokens = (uint64_t)(total->valuedouble > 0 ? total->valuedouble : 0);
    }

    /* 成本（USD）：llm_d 按模型单价估算后透出 */
    if (out_cost) {
        cJSON *cost = result ? cJSON_GetObjectItem(result, "cost_usd") : NULL;
        if (cJSON_IsNumber(cost))
            *out_cost = cost->valuedouble;
    }

    cJSON_Delete(root);
    return 0;
}

/* ==================== 工具循环辅助 ==================== */

/**
 * @brief 从 llm_d 响应提取 tool_calls（choices[0].tool_calls）
 * @return 0 且有 tool_calls（*out 需调用者 cJSON_Delete），非 0 无 tool_calls
 */
static int parse_llm_tool_calls(const char *llm_resp, cJSON **out_tool_calls)
{
    *out_tool_calls = NULL;
    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 = (choices && cJSON_GetArraySize(choices) > 0)
                         ? cJSON_GetArrayItem(choices, 0)
                         : NULL;
    cJSON *tc = choice0 ? cJSON_GetObjectItem(choice0, "tool_calls") : NULL;
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
        *out_tool_calls = cJSON_Duplicate(tc, 1);
    }
    cJSON_Delete(root);
    return *out_tool_calls ? 0 : -1;
}

/**
 * @brief 调用 tool_d execute_tool 执行单个工具
 * @param name 工具名（fs_read/fs_write/fs_list/shell_run）
 * @param args_json 工具参数（OpenAI tool_call arguments JSON 字符串）
 * @param out_text 执行结果文本（AIRY_MALLOC，调用者释放）；失败时含错误描述
 * @return 0 成功，非 0 失败
 */
static int gw_execute_tool(const gateway_business_ctx_t *ctx, const char *name,
                           const char *args_json, char **out_text)
{
    *out_text = NULL;

    /* R4 ACL：外部请求不得直接触达未授权工具（shell_run 默认拒绝） */
    if (gw_acl_check_tool(name) != 0) {
        *out_text = AIRY_STRDUP("Permission denied: tool not authorized");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req)
        return -1;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "execute_tool");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    cJSON_AddItemToObject(req, "params", params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return -1;

    char *resp = tool_call_rpc(ctx, req_str);
    AIRY_FREE(req_str);
    if (!resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }

    /* 构造结果文本：优先 output；错误时 "Error: <error>"
     * 返回码语义：0=工具执行成功，非 0=工具层失败（RPC 成功但工具报错/异常） */
    char *text = NULL;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring
                                   ? output->valuestring
                                   : "(no output)");
        } else {
            const char *e = cJSON_IsString(error) && error->valuestring ? error->valuestring
                                                                        : "execution failed";
            size_t elen = strlen(e) + 8;
            text = (char *)AIRY_MALLOC(elen);
            if (text)
                snprintf(text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "unknown";
        size_t elen = strlen(m) + 8;
        text = (char *)AIRY_MALLOC(elen);
        if (text)
            snprintf(text, elen, "Error: %s", m);
    }
    if (!text)
        text = AIRY_STRDUP("Tool execution returned no result");
    *out_text = text;
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/**
 * @brief 构造 llm_d complete JSON-RPC 请求（含 tools 数组透传）
 * @param model 模型名
 * @param messages 对话历史（cJSON 数组，深拷贝进请求）
 * @return JSON 请求字符串（AIRY_MALLOC），失败返回 NULL
 */
static char *gw_build_llm_request(const char *model, const cJSON *messages)
{
    cJSON *llm_req = cJSON_CreateObject();
    if (!llm_req)
        return NULL;
    cJSON_AddStringToObject(llm_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(llm_req, "id", 1);
    cJSON_AddStringToObject(llm_req, "method", "complete");
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(llm_req);
        return NULL;
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", 2048);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);
    cJSON_AddItemToObject(llm_req, "params", llm_params);

    char *req_str = cJSON_PrintUnformatted(llm_req);
    cJSON_Delete(llm_req);
    return req_str;
}

/**
 * @brief 执行 Agent 工具循环：LLM → tool_calls → 执行 → 回填 → 继续（ReAct）
 *
 * 单次 agent.run 的完整推理+工具链路。循环上限 GW_MAX_TOOL_LOOPS 轮防失控；
 * 每轮结束把 assistant（含 tool_calls）与 tool（含 tool_call_id）消息回填
 * 对话历史，供下一轮 LLM 参考。
 *
 * @param ctx 网关上下文
 * @param model 模型名
 * @param prompt 用户输入
 * @param active 运行中请求条目（agent.cancel 支持；轮次间检查取消标志）
 * @param out_trace 工具轨迹数组（cJSON，调用者 cJSON_Delete；失败为 NULL）
 * @param out_text 最终回复文本（AIRY_MALLOC，调用者 AIRY_FREE；失败为 NULL）
 * @param out_tokens 累计 token 用量
 * @param out_cost 累计成本（USD）
 * @return 0 成功（*out_text 有效）；1 用户取消；非 0 失败（未得到最终答案）
 */
static int gw_run_tool_loop(const gateway_business_ctx_t *ctx, const char *model,
                            const char *prompt, gw_active_request_t *active,
                            cJSON **out_trace, char **out_text,
                            uint64_t *out_tokens, double *out_cost)
{
    *out_trace = NULL;
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

    cJSON *messages = cJSON_CreateArray();
    cJSON *tool_trace = cJSON_CreateArray();
    if (!messages || !tool_trace) {
        if (messages)
            cJSON_Delete(messages);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        return -1;
    }
    cJSON *msg0 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg0, "role", "user");
    cJSON_AddStringToObject(msg0, "content", prompt);
    cJSON_AddItemToArray(messages, msg0);

    char *final_text = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    int rc = -1; /* 默认失败：未在循环内得到最终答案 */

    for (int loops = 0; loops < GW_MAX_TOOL_LOOPS; loops++) {
        /* 人工中止：轮次间检查 agent.cancel 置位，立即中断链路 */
        if (gw_active_is_cancelled(active)) {
            LOG_INFO("gateway: agent.run cancelled by user (session=%s)",
                     active ? active->session_id : "?");
            rc = 1;
            break;
        }

        char *llm_req_str = gw_build_llm_request(model, messages);
        if (!llm_req_str) {
            break;
        }
        char *llm_resp = llm_call_complete(ctx, llm_req_str);
        AIRY_FREE(llm_req_str);
        if (!llm_resp) {
            break;
        }

        cJSON *tool_calls = NULL;
        parse_llm_tool_calls(llm_resp, &tool_calls);

        char *text = NULL;
        uint64_t tokens = 0;
        double cost = 0.0;
        parse_llm_result(llm_resp, &text, &tokens, &cost);
        total_tokens += tokens;
        total_cost += cost;

        /* assistant 消息（含 tool_calls）加入对话历史 */
        cJSON *assistant_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
        cJSON_AddStringToObject(assistant_msg, "content", text ? text : "");
        if (tool_calls) {
            cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
        cJSON_AddItemToArray(messages, assistant_msg);

        if (!tool_calls) {
            /* 无工具调用 → 最终答案 */
            final_text = text ? text : AIRY_STRDUP("");
            AIRY_FREE(llm_resp);
            rc = 0;
            break;
        }

        /* assistant 文本已被 cJSON 深拷贝进消息，释放本地副本 */
        AIRY_FREE(text);

        /* 执行每个 tool_call 并回填 */
        int tc_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tc_count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            char *result_text = NULL;
            int erc = gw_execute_tool(ctx, tname, targs, &result_text);

            /* tool 结果消息 */
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", tid);
            cJSON_AddStringToObject(tool_msg, "content",
                                    result_text ? result_text : "Tool execution failed");
            cJSON_AddItemToArray(messages, tool_msg);

            /* 工具轨迹（供 TUI 展示）：ok = 工具真实成败（含工具层失败） */
            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "tool", tname);
            cJSON_AddStringToObject(tr, "arguments", targs);
            cJSON_AddStringToObject(tr, "result", result_text ? result_text : "");
            cJSON_AddNumberToObject(tr, "ok", erc == 0 ? 1 : 0);
            cJSON_AddItemToArray(tool_trace, tr);

            if (result_text)
                AIRY_FREE(result_text);
        }
        /* tool_calls 已复制进 assistant 消息，释放原树（每轮循环一次） */
        cJSON_Delete(tool_calls);
        AIRY_FREE(llm_resp);
    }

    cJSON_Delete(messages);

    if (rc == 0) {
        *out_trace = tool_trace;
        *out_text = final_text;
        *out_tokens = total_tokens;
        if (out_cost)
            *out_cost = total_cost;
    } else {
        if (final_text)
            AIRY_FREE(final_text);
        cJSON_Delete(tool_trace);
    }
    return rc;
}

static char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    /* 提取用户消息：优先 params.prompt，其次 params.messages[0].content */
    const char *prompt = NULL;
    if (params) {
        cJSON *p = cJSON_GetObjectItem(params, "prompt");
        if (cJSON_IsString(p)) {
            prompt = p->valuestring;
        } else {
            cJSON *messages = cJSON_GetObjectItem(params, "messages");
            cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0)
                            ? cJSON_GetArrayItem(messages, 0)
                            : NULL;
            cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
            if (cJSON_IsString(c))
                prompt = c->valuestring;
        }
    }
    if (!prompt || !*prompt) {
        return jsonrpc_error(-32602, "Invalid params: missing prompt", id);
    }

    /* 模型：params.model → env AIRY_AGENT_MODEL → 默认 */
    const char *model = ctx->default_model;
    if (params) {
        cJSON *m = cJSON_GetObjectItem(params, "model");
        if (cJSON_IsString(m) && m->valuestring && *m->valuestring)
            model = m->valuestring;
    }

    /* ── 分支：params.agent 存在 → agent_d 编排（spawn+invoke）；
     *        否则维持 llm_d 直连工具循环（向后兼容，D4） ── */
    cJSON *tool_trace = NULL;
    char *final_text = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;

    /* 会话 ID：客户端可预分配（agent.cancel 需请求前已知 session_id）；
     * 否则网关生成唯一 ID（时间+自增+随机位，非 time(NULL) 伪会话） */
    char session_id[GW_SESSION_ID_LEN];
    cJSON *sid_param = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (cJSON_IsString(sid_param) && sid_param->valuestring && *sid_param->valuestring &&
        strlen(sid_param->valuestring) < GW_SESSION_ID_LEN &&
        strncmp(sid_param->valuestring, "sess_", 5) == 0) {
        AIRY_STRNCPY_TERM(session_id, sid_param->valuestring, sizeof(session_id));
    } else {
        gw_gen_session_id(session_id, sizeof(session_id));
    }

    /* 注册运行中请求（agent.cancel 支持） */
    gw_active_request_t *active = gw_active_register(ctx, session_id);

    cJSON *agent_spec = params ? cJSON_GetObjectItem(params, "agent") : NULL;
    LOG_INFO("gateway: agent.run start (session=%s, model=%s, orchestrate=%d)",
             session_id, model ? model : "(default)", agent_spec ? 1 : 0);
    int run_rc = -1;
    if (agent_spec) {
        char *err_msg = NULL;
        run_rc = gw_agent_run_orchestrate(ctx, agent_spec, prompt,
                                          &final_text, &err_msg);
        if (run_rc != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Agent orchestration failed: %s",
                     err_msg ? err_msg : "unknown error");
            AIRY_FREE(err_msg);
            gw_active_unregister(ctx, active);
            return jsonrpc_error(-32603, msg, id);
        }
        /* 编排路径工具轨迹由 runner（ecosystem/agents）内部完成，网关不感知；
         * 响应中 tool_trace 置空数组保持字段契约 */
        tool_trace = cJSON_CreateArray();
    } else {
        /* 返回值必须检查：工具循环耗尽/LLM 故障/用户取消时，
         * final_text 可能为 NULL，不得静默返回空响应 */
        run_rc = gw_run_tool_loop(ctx, model, prompt, active, &tool_trace, &final_text,
                                  &total_tokens, &total_cost);
    }
    gw_active_unregister(ctx, active);
    LOG_INFO("gateway: agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)",
             session_id, run_rc, (unsigned long long)total_tokens, total_cost);

    if (run_rc == 1) {
        /* 用户中止（Ctrl+X / agent.cancel） */
        cJSON *err_out = cJSON_CreateObject();
        cJSON_AddStringToObject(err_out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(err_out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(err_out, "id");
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32800);
        cJSON_AddStringToObject(err, "message", "Request cancelled by user");
        cJSON_AddStringToObject(err, "data", session_id);
        cJSON_AddItemToObject(err_out, "error", err);
        char *err_str = cJSON_PrintUnformatted(err_out);
        cJSON_Delete(err_out);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (final_text)
            AIRY_FREE(final_text);
        return err_str;
    }
    if (run_rc != 0) {
        /* 工具循环失败：明确报错，而非空响应 */
        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (final_text)
            AIRY_FREE(final_text);
        return jsonrpc_error(-32603,
                             "agent.run failed: tool loop exhausted or LLM service error", id);
    }

    /* 构造 agent.run 响应：result.{session_id,response,tokens_used,cost_usd,tool_trace} */
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "session_id", session_id);
    cJSON_AddStringToObject(result, "response", final_text ? final_text : "");
    cJSON_AddNumberToObject(result, "tokens_used", (double)total_tokens);
    cJSON_AddNumberToObject(result, "cost_usd", total_cost);
    if (tool_trace) {
        cJSON_AddItemToObject(result, "tool_trace", tool_trace);
    } else {
        cJSON_AddItemToObject(result, "tool_trace", cJSON_CreateArray());
    }
    cJSON_AddItemToObject(out, "result", result);

    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (final_text)
        AIRY_FREE(final_text);
    return out_str;
}

/* ==================== 公共接口 ==================== */

/**
 * @brief agent.cancel：人工中止运行中的 agent.run 请求
 *
 * params.session_id → 在运行中请求注册表查找条目并置位 cancelled。
 * 工具循环轮次间检查该标志后中断，返回 -32800 错误给原请求。
 *
 * @return JSON-RPC 响应（成功 result.status="cancelling"；找不到返回错误）
 */
static char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *sid = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring) {
        return jsonrpc_error(-32602, "Invalid params: missing session_id", id);
    }

    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t *entry = ctx->active_requests;
    while (entry) {
        if (strcmp(entry->session_id, sid->valuestring) == 0)
            break;
        entry = entry->next;
    }
    if (entry) {
        atomic_store_explicit(&entry->cancelled, 1, memory_order_relaxed);
        LOG_INFO("gateway: agent.cancel set (session=%s)", sid->valuestring);
    }
    airy_mtx_unlock(&ctx->active_lock);

    if (!entry) {
        LOG_DEBUG("gateway: agent.cancel miss (session=%s, 请求已完成或不存在)",
                  sid->valuestring);
        return jsonrpc_error(-32004, "No active request with given session_id", id);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "cancelling");
    cJSON_AddStringToObject(result, "session_id", sid->valuestring);
    cJSON_AddItemToObject(out, "result", result);
    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return out_str;
}

/* 解析 daemon Unix socket 路径：<ENV_NAME> 覆盖 → airy_runtime_dir()/<sock_name> → <sock_name>
 * 与 daemon 侧单一事实来源一致：airy_runtime_dir() 解析 $AIRY_HOME/run，缺省 ~/.airymaxrt/run */
static void gw_resolve_daemon_sock(char *out, size_t out_size,
                                   const char *env_name, const char *sock_name)
{
    const char *env = getenv(env_name);
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/%s", run_dir, sock_name);
    } else {
        AIRY_STRNCPY_TERM(out, sock_name, out_size);
    }
}

gateway_business_ctx_t *gateway_business_ctx_create(void)
{
    gateway_business_ctx_t *ctx =
        (gateway_business_ctx_t *)AIRY_CALLOC(1, sizeof(gateway_business_ctx_t));
    if (!ctx)
        return NULL;

    /* 运行中请求注册表锁（agent.cancel 支持） */
    airy_mtx_init(&ctx->active_lock);
    ctx->active_requests = NULL;

    /* 各 daemon socket：<DAEMON>_SOCK env → $AIRY_RUNTIME_DIR/<name>.sock → $AIRY_HOME/run/<name>.sock */
    gw_resolve_daemon_sock(ctx->llm_sock_path, sizeof(ctx->llm_sock_path),
                           "AIRY_LLM_SOCK", "llm.sock");
    gw_resolve_daemon_sock(ctx->tool_sock_path, sizeof(ctx->tool_sock_path),
                           "AIRY_TOOL_SOCK", "tool.sock");
    gw_resolve_daemon_sock(ctx->agent_sock_path, sizeof(ctx->agent_sock_path),
                           "AIRY_AGENT_SOCK", "agent.sock");
    gw_resolve_daemon_sock(ctx->mem_sock_path, sizeof(ctx->mem_sock_path),
                           "AIRY_MEM_SOCK", "mem.sock");
    gw_resolve_daemon_sock(ctx->sched_sock_path, sizeof(ctx->sched_sock_path),
                           "AIRY_SCHED_SOCK", "sched.sock");

    const char *tcp_env = getenv("AIRY_LLM_TCP_ADDR");
    AIRY_STRNCPY_TERM(ctx->llm_tcp_addr,
                      (tcp_env && *tcp_env) ? tcp_env : "127.0.0.1",
                      sizeof(ctx->llm_tcp_addr));
    const char *port_env = getenv("AIRY_LLM_TCP_PORT");
    ctx->llm_tcp_port = (port_env && *port_env) ? (uint16_t)atoi(port_env) : GW_LLM_DEFAULT_TCP_PORT;

    /* 默认模型：env AIRY_AGENT_MODEL > 用户覆盖 $AIRY_CONFIG_DIR/model.yaml
     * global.default_model > 内置默认（与 model.yaml 对齐）。
     * 用户无需改动仓库 SSoT，仅需在 $AIRY_HOME/config/model.yaml 覆盖
     * global 段即可同时作用于 gateway 与 llm_d（同一解析路径）。 */
    const char *model_env = getenv("AIRY_AGENT_MODEL");
    if (model_env && *model_env) {
        AIRY_STRNCPY_TERM(ctx->default_model, model_env, sizeof(ctx->default_model));
    } else {
        char um[128] = {0};
        const char *cfg_dir = airy_config_dir();
        int has_user_cfg = 0;
        if (cfg_dir) {
            char user_path[1024];
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path)) {
                FILE *uf = fopen(user_path, "rb");
                if (uf) {
                    fclose(uf);
                    if (svc_model_defaults_from_yaml(user_path, um, sizeof(um), NULL, 0) == 0 &&
                        um[0])
                        has_user_cfg = 1;
                }
            }
        }
        AIRY_STRNCPY_TERM(ctx->default_model,
                          has_user_cfg ? um : GW_LLM_DEFAULT_MODEL,
                          sizeof(ctx->default_model));
    }

    return ctx;
}

void gateway_business_ctx_destroy(gateway_business_ctx_t *ctx)
{
    if (!ctx)
        return;

    /* 清理残留的运行中请求注册表（正常流程已注销；防御性释放） */
    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t *entry = ctx->active_requests;
    ctx->active_requests = NULL;
    airy_mtx_unlock(&ctx->active_lock);
    while (entry) {
        gw_active_request_t *next = entry->next;
        AIRY_FREE(entry);
        entry = next;
    }
    airy_mtx_destroy(&ctx->active_lock);
    AIRY_FREE(ctx);
}

char *gateway_business_handle(void *request, void *user_data)
{
    const char *req = (const char *)request;
    gateway_business_ctx_t *ctx = (gateway_business_ctx_t *)user_data;
    if (!req || !ctx) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    cJSON *root = cJSON_Parse(req);
    if (!root) {
        return jsonrpc_error(-32700, "Parse error", NULL);
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return jsonrpc_error(-32600, "Invalid Request", NULL);
    }

    char *resp = NULL;
    if (strcmp(method->valuestring, "agent.run") == 0) {
        resp = handle_agent_run(root, ctx);
    } else if (strcmp(method->valuestring, "agent.cancel") == 0) {
        resp = handle_agent_cancel(root, ctx);
    } else if (strcmp(method->valuestring, "llm.list_models") == 0) {
        resp = handle_llm_list_models(root, ctx);
    } else if (strncmp(method->valuestring, "mem.", 4) == 0) {
        resp = handle_mem_call(root, ctx);
    } else if (strcmp(method->valuestring, "ping") == 0) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(out, "id");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        cJSON_AddItemToObject(out, "result", result);
        resp = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
    } else {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        resp = jsonrpc_error(-32601, "Method not found", id);
    }

    cJSON_Delete(root);
    return resp;
}

/* ==================== Phase 2: 协议适配 backend（内部服务调用） ==================== */

/**
 * @brief MCP 工具执行 backend：tools/call → tool_d.execute_tool
 *
 * 返回的 result_json 为合法 JSON 字符串（带引号，供 MCP tools/call 的
 * content[].text 直接 %s 嵌入），内容为 tool_d 执行输出或错误描述。
 */
int gw_biz_tool_exec(const char *tool_name, const char *arguments_json,
                     char **result_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *result_json = NULL;
    if (!ctx || !tool_name) {
        *result_json = AIRY_STRDUP("\"Invalid tool request\"");
        return -1;
    }

    /* R4 ACL：外部协议（MCP tools/call）不得直接触达未授权工具 */
    if (gw_acl_check_tool(tool_name) != 0) {
        *result_json = AIRY_STRDUP("\"Permission denied: tool not authorized\"");
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }
    cJSON_AddStringToObject(params, "tool_id", tool_name);
    cJSON *pargs = cJSON_Parse(arguments_json && arguments_json[0] ? arguments_json : "{}");
    cJSON_AddItemToObject(params, "params", pargs ? pargs : cJSON_CreateObject());
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }

    char *resp = gw_svc_call(ctx->tool_sock_path, "execute_tool", params_str,
                             GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        *result_json = AIRY_STRDUP("\"Tool service unreachable\"");
        return -1;
    }

    char *text = NULL;
    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *result_json = AIRY_STRDUP("\"Tool service returned invalid response\"");
        return -1;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        if (cJSON_IsNumber(success) && success->valueint != 0 && cJSON_IsString(output)) {
            text = AIRY_STRDUP(output->valuestring);
        } else if (cJSON_IsString(error) && error->valuestring) {
            size_t n = strlen(error->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", error->valuestring);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            size_t n = strlen(msg->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", msg->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!text)
        text = AIRY_STRDUP("(no output)");

    /* 序列化为合法 JSON 字符串（供 MCP %s 嵌入） */
    cJSON *jstr = cJSON_CreateString(text);
    AIRY_FREE(text);
    *result_json = jstr ? cJSON_PrintUnformatted(jstr) : AIRY_STRDUP("\"\"");
    if (jstr)
        cJSON_Delete(jstr);
    return 0;
}

/**
 * @brief OpenAI LLM backend：chat/completions → llm_d.complete
 *
 * 调用 llm_d 后把响应转换为 OpenAI chat.completion 格式
 * （choices[0].message.content / tool_calls），客户端无需感知内部 JSON-RPC。
 */
int gw_biz_llm_complete(const char *model, const char *messages_json,
                        const char *functions_json, double temperature, int max_tokens,
                        char **response_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *response_json = NULL;
    if (!ctx) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "model",
                            (model && model[0]) ? model : ctx->default_model);
    cJSON *msgs = cJSON_Parse(messages_json && messages_json[0] ? messages_json : "[]");
    cJSON_AddItemToObject(params, "messages", msgs ? msgs : cJSON_CreateArray());
    /* OpenAI tools/functions 数组 → llm_d tools（透传，llm_d 已支持 function calling） */
    if (functions_json && functions_json[0]) {
        cJSON *tools = cJSON_Parse(functions_json);
        if (tools) {
            cJSON_AddItemToObject(params, "tools", tools);
        } else {
            cJSON_AddItemToObject(params, "tools", cJSON_CreateArray());
        }
    }
    cJSON_AddNumberToObject(params, "max_tokens", max_tokens > 0 ? max_tokens : 2048);
    cJSON_AddNumberToObject(params, "temperature", temperature);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char *resp = gw_svc_call(ctx->llm_sock_path, "complete", params_str,
                             GW_LLM_DEFAULT_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        /* 透传 llm_d 错误 */
        *response_json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 = (choices && cJSON_GetArraySize(choices) > 0)
                         ? cJSON_GetArrayItem(choices, 0)
                         : NULL;

    cJSON *openai = cJSON_CreateObject();
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "chatcmpl-%ld", (long)time(NULL));
    cJSON_AddStringToObject(openai, "id", idbuf);
    cJSON_AddStringToObject(openai, "object", "chat.completion");
    cJSON_AddNumberToObject(openai, "created", (double)time(NULL));
    cJSON_AddStringToObject(openai, "model",
                            (model && model[0]) ? model : ctx->default_model);
    cJSON *choices_out = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    if (choice0) {
        cJSON *content = cJSON_GetObjectItem(choice0, "content");
        cJSON_AddStringToObject(message, "content",
                                cJSON_IsString(content) ? content->valuestring : "");
        cJSON *tool_calls = cJSON_GetObjectItem(choice0, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON_AddItemToObject(message, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
        cJSON *reason = cJSON_GetObjectItem(choice0, "finish_reason");
        cJSON_AddStringToObject(choice, "finish_reason",
                                cJSON_IsString(reason) ? reason->valuestring : "stop");
    } else {
        cJSON_AddStringToObject(message, "content", "");
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices_out, choice);
    cJSON_AddItemToObject(openai, "choices", choices_out);
    if (result) {
        cJSON *usage = cJSON_GetObjectItem(result, "usage");
        if (cJSON_IsObject(usage)) {
            cJSON_AddItemToObject(openai, "usage", cJSON_Duplicate(usage, 1));
        }
    }

    *response_json = cJSON_PrintUnformatted(openai);
    cJSON_Delete(openai);
    cJSON_Delete(root);
    return *response_json ? 0 : -1;
}

/**
 * @brief A2A 任务 backend：task → sched_d.schedule_task
 *
 * 输出为调度结果 JSON（selected_agent_id/confidence/estimated_time_ms），
 * 供 A2A task 响应的 output 字段直接嵌入。
 */
int gw_biz_sched_schedule(const char *task_id, const char *task_type,
                          const char *input_json, char **output_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *output_json = NULL;
    if (!ctx || !task_id) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "task_id", task_id);
    char desc[512];
    snprintf(desc, sizeof(desc), "A2A delegated task (type=%s)",
             task_type ? task_type : "unknown");
    cJSON_AddStringToObject(task, "task_description", desc);
    cJSON_AddNumberToObject(task, "priority", 0);
    cJSON_AddNumberToObject(task, "timeout_ms", 30000);
    /* 携带 A2A 原始输入，供调度器/派发参考 */
    if (input_json && input_json[0]) {
        cJSON *input = cJSON_Parse(input_json);
        if (input) {
            cJSON_AddItemToObject(task, "input", input);
        }
    }
    cJSON_AddItemToObject(params, "task", task);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char *resp = gw_svc_call(ctx->sched_sock_path, "schedule_task", params_str,
                             GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    int rc = -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        *output_json = cJSON_PrintUnformatted(result);
        rc = *output_json ? 0 : -1;
    } else {
        cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        const char *m = (err && cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring
                                                                         : "schedule failed";
        size_t n = strlen(m) + 32;
        char *ebuf = (char *)AIRY_MALLOC(n);
        if (ebuf) {
            snprintf(ebuf, n, "{\"error\":\"%s\"}", m);
            *output_json = ebuf;
        }
    }
    cJSON_Delete(root);
    return rc;
}

/* ==================== 统一协议入口 ==================== */

/* MCP JSON-RPC 方法集（initialize 等请求体无 tools/ 关键字，用于兜底识别） */
static int is_mcp_jsonrpc_method(const char *method)
{
    static const char *mcp_methods[] = {
        "initialize", "tools/list", "tools/call",
        "resources/list", "resources/read", "prompts/list",
        "notifications/initialized", NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; mcp_methods[i]; i++) {
        if (strcmp(method, mcp_methods[i]) == 0)
            return 1;
    }
    return strncmp(method, "mcp.", 4) == 0;
}

/* A2A JSON-RPC 方法集（tasks 系 / message 系，A2A 基于 JSON-RPC 2.0） */
static int is_a2a_jsonrpc_method(const char *method)
{
    static const char *a2a_methods[] = {
        "tasks/send", "tasks/get", "tasks/cancel", "tasks/pushNotification",
        "message/send", "agent-card/get", "agent/getAgentCard", NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; a2a_methods[i]; i++) {
        if (strcmp(method, a2a_methods[i]) == 0)
            return 1;
    }
    return strncmp(method, "a2a.", 4) == 0;
}

char *gateway_protocol_entry(void *request, void *user_data)
{
    const char *body = (const char *)request;
    const gateway_entry_ctx_t *ectx = (const gateway_entry_ctx_t *)user_data;
    if (!body || !ectx || !ectx->biz_ctx || !ectx->router) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    /* 1. 协议检测（body-only；path 在 HTTP handler 层不可用） */
    gw_proto_detect_result_t proto = gw_proto_detect(NULL, NULL, body);

    /* 2. JSONRPC 兜底：MCP/A2A 方法集（initialize / tasks/send 等） */
    if (proto == GW_PROTO_DETECT_JSONRPC) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *m = cJSON_GetObjectItem(root, "method");
            if (cJSON_IsString(m)) {
                if (is_mcp_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_MCP;
                } else if (is_a2a_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_A2A;
                }
            }
            cJSON_Delete(root);
        }
    }

    /* 3. 协议适配器路由（MCP/OpenAI/A2A） */
    if (proto == GW_PROTO_DETECT_MCP || proto == GW_PROTO_DETECT_OPENAI ||
        proto == GW_PROTO_DETECT_A2A) {
        char *resp = NULL;
        int rc = gw_proto_router_route((gw_proto_router_t *)ectx->router, proto,
                                       "POST", NULL, body, &resp);
        if (rc != 0 || !resp) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Protocol handler failed: proto=%d rc=%d",
                     (int)proto, rc);
            return jsonrpc_error(-32603, msg, NULL);
        }
        return resp;
    }

    /* 4. JSON-RPC 业务（agent.run / ping）→ 原业务处理器 */
    return gateway_business_handle(request, ectx->biz_ctx);
}
