/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_verify.c
 * @brief 完成判据复核实现（0.1.21 C-17）。
 *
 * 四类判据各自独立取证，逐项回传 expect/actual/pass，供 agent_d 在终局
 * 前裁决。本文件只做「如实复核」：不读模型上下文、不依赖自报完成，
 * 也不改写工作区。命令判据复用 shell 执行机制（同一超时/沙箱策略），
 * 文件判据走 fopen/stat 而非 shell test，服务判据做本机 TCP 连接，
 * RPC 判据只允许只读方法白名单（execute/poll 等一律拒绝）。
 */

#include "verify/tool_verify.h"

#include "airy_memory.h"
#include "airy_sandbox.h"
#include "airy_verify.h"
#include "builtin/builtin_exec_internal.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "platform_paths.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define V_DETAIL 240
#define V_CHUNK 8192
#define V_READ_CAP (4u * 1024u * 1024u)
#define V_CONNECT_MS 1000

typedef struct {
    char expect[V_DETAIL];
    char actual[V_DETAIL];
    int pass;
} vres_t;

#ifdef _WIN32
typedef SOCKET vsock_t;
#define V_SOCK_BAD INVALID_SOCKET
#else
typedef int vsock_t;
#define V_SOCK_BAD (-1)
#endif

/* 截断到 cap-1 且不切断 UTF-8 续字节：判据证据会原样呈现给用户。 */
static void v_set(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    AIRY_MEMCPY(dst, src, n);
    dst[n] = '\0';
}

/* 命令证据只保留最后一行非空输出：失败原因通常在那里。 */
static void v_tail(const char *out, char *dst, size_t cap)
{
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!out || !out[0])
        return;
    const char *end = out + strlen(out);
    while (end > out && strchr("\r\n \t", end[-1]))
        end--;
    const char *start = end;
    while (start > out && start[-1] != '\n')
        start--;
    size_t n = (size_t)(end - start);
    if (n >= V_DETAIL)
        start += n - V_DETAIL;
    v_set(dst, cap, start);
}

#ifdef _WIN32
static int v_sock_ready(void)
{
    static int once;
    WSADATA w;
    if (!once && WSAStartup(MAKEWORD(2, 2), &w) != 0)
        return -1;
    once = 1;
    return 0;
}

static int v_nonblock(vsock_t s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
}

static int v_pending(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static int v_wait(vsock_t s, int ms)
{
    fd_set wf;
    struct timeval tv;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(0, NULL, &wf, NULL, &tv);
}

static void v_close(vsock_t s)
{
    closesocket(s);
}
#else
static int v_sock_ready(void)
{
    return 0;
}

static int v_nonblock(vsock_t s)
{
    int fl = fcntl(s, F_GETFL, 0);
    return (fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0) ? 0 : -1;
}

static int v_pending(void)
{
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
}

static int v_wait(vsock_t s, int ms)
{
    fd_set wf;
    struct timeval tv;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(s + 1, NULL, &wf, NULL, &tv);
}

static void v_close(vsock_t s)
{
    close(s);
}
#endif

static int v_port_open(const char *host, int port, int timeout_ms)
{
    if (v_sock_ready() != 0)
        return 0;
    struct sockaddr_in sa;
    AIRY_MEMSET(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
        return 0;

    vsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == V_SOCK_BAD)
        return 0;
    int ok = 0;
    if (v_nonblock(s) != 0) {
        v_close(s);
        return 0;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        ok = 1;
    } else if (v_pending() && v_wait(s, timeout_ms > 0 ? timeout_ms : V_CONNECT_MS) > 0) {
        int so_err = 0;
#ifdef _WIN32
        int len = (int)sizeof(so_err);
#else
        socklen_t len = sizeof(so_err);
#endif
        ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len) == 0 && so_err == 0;
    }
    v_close(s);
    return ok;
}

static int v_budget(int timeout_ms)
{
    if (timeout_ms <= 0)
        return 10000;
    if (timeout_ms > (int)AIRY_VERIFY_MAX_TIMEOUT_MS)
        return (int)AIRY_VERIFY_MAX_TIMEOUT_MS;
    return timeout_ms;
}

static void v_cmd(const cJSON *item, int budget, vres_t *r)
{
    cJSON *jc = cJSON_GetObjectItem(item, AIRY_VC_CMD);
    v_set(r->expect, sizeof(r->expect), "exit_code=0");
    if (!cJSON_IsString(jc) || !jc->valuestring || !jc->valuestring[0]) {
        v_set(r->actual, sizeof(r->actual), "missing cmd");
        return;
    }
    os_sandbox_cfg_t sandbox;
    os_sandbox_cfg_from_env(&sandbox);
    char *out = NULL;
    int code = -1;
    int trunc = 0;
    int rc = builtin_shell_run(jc->valuestring, NULL, &out, &code, (uint32_t)budget, &trunc,
                               &sandbox, 1);
    if (rc != 0) {
        v_set(r->actual, sizeof(r->actual), "spawn failed");
    } else {
        char tail[V_DETAIL];
        v_tail(out, tail, sizeof(tail));
        r->pass = (code == 0);
        snprintf(r->actual, sizeof(r->actual), "exit_code=%d%s%s", code, tail[0] ? " last=" : "",
                 tail);
    }
    AIRY_FREE(out);
}

static int v_contains(FILE *f, const char *needle, size_t *read_bytes)
{
    size_t nl = strlen(needle);
    size_t keep = nl > 1 ? nl - 1 : 0;
    char buf[V_CHUNK + V_DETAIL];
    size_t held = 0;
    int found = 0;
    if (keep >= V_DETAIL)
        return 0;
    while (*read_bytes < V_READ_CAP) {
        size_t got = fread(buf + held, 1, V_CHUNK, f);
        if (got == 0)
            break;
        *read_bytes += got;
        size_t len = held + got;
        buf[len] = '\0';
        if (strstr(buf, needle)) {
            found = 1;
            break;
        }
        held = nl > 1 ? (nl - 1) : 0;
        if (held > len)
            held = len;
        AIRY_MEMMOVE(buf, buf + len - held, held);
    }
    return found;
}

static void v_file(const cJSON *item, vres_t *r)
{
    cJSON *jp = cJSON_GetObjectItem(item, AIRY_VC_PATH);
    cJSON *jc = cJSON_GetObjectItem(item, AIRY_VC_CONTAINS);
    const char *path = cJSON_IsString(jp) ? jp->valuestring : NULL;
    const char *needle = cJSON_IsString(jc) ? jc->valuestring : NULL;
    if (!path || !path[0]) {
        v_set(r->expect, sizeof(r->expect), "path exists");
        v_set(r->actual, sizeof(r->actual), "missing path");
        return;
    }
    if (!needle || !needle[0]) {
        v_set(r->expect, sizeof(r->expect), "path exists");
        FILE *f = fopen(path, "rb");
        r->pass = (f != NULL);
        if (f)
            fclose(f);
        snprintf(r->actual, sizeof(r->actual), "%s: %s", path, r->pass ? "exists" : "missing");
        return;
    }
    snprintf(r->expect, sizeof(r->expect), "contains=%s", needle);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(r->actual, sizeof(r->actual), "%s: missing", path);
        return;
    }
    size_t read_bytes = 0;
    r->pass = v_contains(f, needle, &read_bytes);
    fclose(f);
    snprintf(r->actual, sizeof(r->actual), "read=%uB match=%s", (unsigned)read_bytes,
             r->pass ? "yes" : "no");
}

static void v_service(const cJSON *item, vres_t *r)
{
    cJSON *jport = cJSON_GetObjectItem(item, AIRY_VC_PORT);
    cJSON *jhost = cJSON_GetObjectItem(item, AIRY_VC_HOST);
    const char *host = cJSON_IsString(jhost) && jhost->valuestring ? jhost->valuestring
                                                                  : AIRY_VERIFY_DEFAULT_HOST;
    int port = cJSON_IsNumber(jport) ? jport->valueint : 0;
    if (port <= 0 || port > 65535) {
        v_set(r->expect, sizeof(r->expect), "port in 1..65535");
        v_set(r->actual, sizeof(r->actual), "missing/invalid port");
        return;
    }
    snprintf(r->expect, sizeof(r->expect), "tcp %s:%d reachable", host, port);
    r->pass = v_port_open(host, port, V_CONNECT_MS);
    v_set(r->actual, sizeof(r->actual), r->pass ? "connected" : "unreachable");
}

/* 只读白名单：判据复核不得有副作用，写型/审批型方法一律不进入。 */
static const char *const k_ro_methods[] = {
    "list_tools", "list", "health_check", "get_stats", "status", "pending", "jobs", "turns",
    "vocab", "count",
};

static int v_method_ro(const char *m)
{
    for (size_t i = 0; i < sizeof(k_ro_methods) / sizeof(k_ro_methods[0]); i++) {
        if (strcmp(m, k_ro_methods[i]) == 0)
            return 1;
    }
    return 0;
}

static int v_daemon_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > 32)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static void v_rpc(const cJSON *item, int budget, vres_t *r)
{
    cJSON *jd = cJSON_GetObjectItem(item, AIRY_VC_DAEMON);
    cJSON *jm = cJSON_GetObjectItem(item, AIRY_VC_METHOD);
    const char *daemon = cJSON_IsString(jd) ? jd->valuestring : NULL;
    const char *method = cJSON_IsString(jm) ? jm->valuestring : NULL;
    if (!v_daemon_name(daemon) || !method || !method[0]) {
        v_set(r->expect, sizeof(r->expect), "daemon name + method");
        v_set(r->actual, sizeof(r->actual), "missing/invalid daemon or method");
        return;
    }
    snprintf(r->expect, sizeof(r->expect), "rpc %s.%s ok", daemon, method);
    if (!v_method_ro(method)) {
        v_set(r->actual, sizeof(r->actual), "method not in read-only allowlist");
        return;
    }
    char sock[256];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket(daemon));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, method, "{}", &resp, (uint32_t)budget);
    AIRY_FREE(resp);
    r->pass = (rc == AIRY_SUCCESS);
    snprintf(r->actual, sizeof(r->actual), "rc=%d", rc);
}

static cJSON *v_item(const char *type, const vres_t *r)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, AIRY_VC_TYPE, type);
    cJSON_AddStringToObject(o, AIRY_VK_EXPECT, r->expect);
    cJSON_AddStringToObject(o, AIRY_VK_ACTUAL, r->actual);
    cJSON_AddBoolToObject(o, AIRY_VK_PASS, r->pass ? 1 : 0);
    return o;
}

static cJSON *v_eval(const cJSON *item, int budget)
{
    vres_t r;
    AIRY_MEMSET(&r, 0, sizeof(r));
    if (!cJSON_IsObject(item)) {
        v_set(r.expect, sizeof(r.expect), "criteria item object");
        v_set(r.actual, sizeof(r.actual), "not an object");
        return v_item("unknown", &r);
    }
    cJSON *jt = cJSON_GetObjectItem(item, AIRY_VC_TYPE);
    const char *type = cJSON_IsString(jt) && jt->valuestring ? jt->valuestring : "";
    if (strcmp(type, AIRY_VC_T_CMD) == 0)
        v_cmd(item, budget, &r);
    else if (strcmp(type, AIRY_VC_T_FILE) == 0)
        v_file(item, &r);
    else if (strcmp(type, AIRY_VC_T_SERVICE) == 0)
        v_service(item, &r);
    else if (strcmp(type, AIRY_VC_T_RPC) == 0)
        v_rpc(item, budget, &r);
    else {
        v_set(r.expect, sizeof(r.expect), "type in {cmd,file,service,rpc}");
        v_set(r.actual, sizeof(r.actual), type[0] ? type : "missing type");
    }
    return v_item(type[0] ? type : "unknown", &r);
}

static cJSON *v_fail(const char *why)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON *items = cJSON_CreateArray();
    if (!items) {
        cJSON_Delete(resp);
        return NULL;
    }
    cJSON_AddBoolToObject(resp, AIRY_VK_OK, 0);
    cJSON_AddNumberToObject(resp, AIRY_VK_TOTAL, 0);
    cJSON_AddNumberToObject(resp, AIRY_VK_PASSED, 0);
    cJSON_AddItemToObject(resp, AIRY_VK_ITEMS, items);
    cJSON_AddStringToObject(resp, AIRY_VK_ERROR, why);
    return resp;
}

cJSON *tool_verify_run(const cJSON *criteria, int timeout_ms)
{
    if (!cJSON_IsArray(criteria))
        return v_fail("criteria must be a non-empty array");
    int n = cJSON_GetArraySize(criteria);
    if (n <= 0)
        return v_fail("criteria must be a non-empty array");
    if (n > AIRY_VERIFY_MAX_ITEMS)
        return v_fail("too many criteria (max 16)");

    cJSON *resp = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!resp || !items) {
        cJSON_Delete(resp);
        cJSON_Delete(items);
        return NULL;
    }
    cJSON_AddItemToObject(resp, AIRY_VK_ITEMS, items);
    int budget = v_budget(timeout_ms);
    int passed = 0;
    for (int i = 0; i < n; i++) {
        cJSON *one = v_eval(cJSON_GetArrayItem(criteria, i), budget);
        if (!one)
            continue;
        cJSON *p = cJSON_GetObjectItem(one, AIRY_VK_PASS);
        if (cJSON_IsTrue(p))
            passed++;
        cJSON_AddItemToArray(items, one);
    }
    cJSON_AddBoolToObject(resp, AIRY_VK_OK, passed == n);
    cJSON_AddNumberToObject(resp, AIRY_VK_TOTAL, n);
    cJSON_AddNumberToObject(resp, AIRY_VK_PASSED, passed);
    SVC_LOG_INFO("tool.verify: total=%d passed=%d", n, passed);
    return resp;
}
