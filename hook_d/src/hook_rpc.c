// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file hook_rpc.c
 * @brief hook.* RPC 方法域：Hook 注册表读写、事件链触发与聚合决策、
 *        P1-5 会话级上下文注入通道、健康与统计面。
 *
 * 由 main.c 按单一职责拆分（0.1.19 gen5 装配）：method_fn 薄壳 m_* 把
 * 机制层注入的 &client_fd 翻译为协议无关的 handler（cJSON*, int,
 * airy_sock_t）。数据源为 airy_coreloop_hooks 的 hook_registry；
 * 会话存储的生命周期与访问原语在 svc.c。
 */

#include "airy_memory.h"
#include "error.h"
#include "hook_d_internal.h"
#include "svc_hook_d.h"

#include "airy_hook.h"
#include "hook_registry.h"
#include "hook_service.h"
#include "svc_logger.h"

#include <string.h>
#include <time.h>

static void handle_health(int id, airy_sock_t fd);
static void handle_ping(int id, airy_sock_t fd);
static void handle_status(int id, airy_sock_t fd);
static void handle_list(int id, airy_sock_t fd);
static void handle_stats(cJSON *params, int id, airy_sock_t fd);
static void handle_register(cJSON *params, int id, airy_sock_t fd);
static void handle_unregister(cJSON *params, int id, airy_sock_t fd);
static void handle_trigger(cJSON *params, int id, airy_sock_t fd);
static void handle_session_start(cJSON *params, int id, airy_sock_t fd);
static void handle_session_get(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);

static const char *hook_decision_name(hook_decision_t decision)
{
    switch (decision) {
    case HOOK_DECISION_SKIP:
        return "skip";
    case HOOK_DECISION_RETRY:
        return "retry";
    case HOOK_DECISION_ABORT:
        return "abort";
    case HOOK_DECISION_MODIFY:
        return "modify";
    default:
        return "continue";
    }
}

static const char *hook_type_name(hook_type_t type)
{
    /* P1-5：类型名单一权威源（airy_hook_type_name），禁止本域重复
     * 维护类型名数组（漂移即越界）。 */
    return airy_hook_type_name(type);
}

static int hook_type_from_name(const char *name)
{
    if (!name)
        return -1;
    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        if (strcmp(name, hook_type_name((hook_type_t)t)) == 0)
            return t;
    }
    return -1;
}

void m_health(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_health(id, *(airy_sock_t *)user_data);
}

void m_ping(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_ping(id, *(airy_sock_t *)user_data);
}

void m_status(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_status(id, *(airy_sock_t *)user_data);
}

void m_list(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_list(id, *(airy_sock_t *)user_data);
}

void m_stats(cJSON *params, int id, void *user_data)
{
    handle_stats(params, id, *(airy_sock_t *)user_data);
}

void m_register(cJSON *params, int id, void *user_data)
{
    handle_register(params, id, *(airy_sock_t *)user_data);
}

void m_unregister(cJSON *params, int id, void *user_data)
{
    handle_unregister(params, id, *(airy_sock_t *)user_data);
}

void m_trigger(cJSON *params, int id, void *user_data)
{
    handle_trigger(params, id, *(airy_sock_t *)user_data);
}

void m_session_start(cJSON *params, int id, void *user_data)
{
    handle_session_start(params, id, *(airy_sock_t *)user_data);
}

void m_session_get(cJSON *params, int id, void *user_data)
{
    handle_session_get(params, id, *(airy_sock_t *)user_data);
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_health_check(id, *(airy_sock_t *)user_data);
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void handle_health(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "healthy", g_registry_initialized ? true : false);
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_ping(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "uptime_sec", (double)(time(NULL) - (time_t)g_start_time));
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_status(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "hook_d");
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    cJSON_AddBoolToObject(result, "registry_initialized", g_registry_initialized ? true : false);

    cJSON *by_type = cJSON_CreateObject();
    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        cJSON_AddNumberToObject(by_type, hook_type_name((hook_type_t)t),
                                (double)hook_registry_count_by_type((hook_type_t)t));
    }
    cJSON_AddItemToObject(result, "by_type", by_type);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_list(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *hooks = cJSON_CreateArray();

    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        hook_entry_t *entries[HOOK_REGISTRY_MAX];
        size_t count = 0;
        if (hook_registry_get_by_type((hook_type_t)t, entries, HOOK_REGISTRY_MAX, &count) != 0)
            continue;
        for (size_t i = 0; i < count; i++) {
            const hook_entry_t *e = entries[i];
            cJSON *h = cJSON_CreateObject();
            cJSON_AddStringToObject(h, "name", e->name);
            cJSON_AddStringToObject(h, "type", hook_type_name(e->type));
            cJSON_AddNumberToObject(h, "type_id", (double)e->type);
            cJSON_AddNumberToObject(h, "impl_type", (double)e->impl_type);
            cJSON_AddNumberToObject(h, "priority", (double)e->priority);
            cJSON_AddBoolToObject(h, "enabled", e->enabled);
            cJSON_AddNumberToObject(h, "invoke_count", (double)e->invoke_count);
            cJSON_AddNumberToObject(h, "skip_count", (double)e->skip_count);
            cJSON_AddNumberToObject(h, "abort_count", (double)e->abort_count);
            cJSON_AddNumberToObject(h, "total_duration_ns", (double)e->total_duration_ns);
            if (e->script_path[0])
                cJSON_AddStringToObject(h, "script_path", e->script_path);
            cJSON_AddItemToArray(hooks, h);
        }
    }
    cJSON_AddItemToObject(result, "hooks", hooks);
    cJSON_AddNumberToObject(result, "count", (double)hook_registry_count());
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_stats(cJSON *params, int id, airy_sock_t fd)
{
    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }
    hook_stats_t stats;
    if (hook_registry_get_stats(name, &stats) != 0) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_METHOD_NOT_FOUND, "Hook not found", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddNumberToObject(result, "invoke_count", (double)stats.invoke_count);
    cJSON_AddNumberToObject(result, "skip_count", (double)stats.skip_count);
    cJSON_AddNumberToObject(result, "abort_count", (double)stats.abort_count);
    cJSON_AddNumberToObject(result, "retry_count", (double)stats.retry_count);
    cJSON_AddNumberToObject(result, "modify_count", (double)stats.modify_count);
    cJSON_AddNumberToObject(result, "total_duration_ns", (double)stats.total_duration_ns);
    cJSON_AddNumberToObject(result, "max_duration_ns", (double)stats.max_duration_ns);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "hook_d");
    cJSON_AddNumberToObject(result, "hooks", (double)hook_registry_count());
    if (g_start_time > 0) {
        cJSON_AddNumberToObject(result, "uptime_s", (double)((uint64_t)time(NULL) - g_start_time));
    }
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

/*
 * hook.register: 将脚本型 Hook 注册进 hook_registry（RPC 无法传递 C
 * 回调，故仅支持 shell/python/webhook 实现类型；CALLBACK 限内置 handler）。
 * params: name(必填), type(字符串或整数, 如 "pre_exec"),
 * impl("shell"/"python"/"webhook"), script_path(脚本路径或 Webhook URL),
 * priority(缺省 0), enabled(缺省 true)
 */
static void handle_register(cJSON *params, int id, airy_sock_t fd)
{
    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }

    int type = -1;
    cJSON *type_json = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsNumber(type_json)) {
        type = type_json->valueint;
    } else if (cJSON_IsString(type_json)) {
        type = hook_type_from_name(type_json->valuestring);
    }
    if (type < 0 || type >= HOOK_TYPE_COUNT) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Invalid hook type", id);
        return;
    }

    const char *impl_str = jsonrpc_get_string_param(params, "impl", "shell");
    hook_impl_type_t impl_type = HOOK_IMPL_SHELL;
    if (strcmp(impl_str, "python") == 0)
        impl_type = HOOK_IMPL_PYTHON;
    else if (strcmp(impl_str, "webhook") == 0)
        impl_type = HOOK_IMPL_WEBHOOK;
    else if (strcmp(impl_str, "callback") == 0)
        impl_type = HOOK_IMPL_CALLBACK;

    const char *script_path = jsonrpc_get_string_param(params, "script_path", NULL);
    if (impl_type != HOOK_IMPL_CALLBACK && (!script_path || !script_path[0])) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS,
                           "Missing script_path for script/webhook hook", id);
        return;
    }

    int priority = 0;
    cJSON *priority_json = cJSON_GetObjectItem(params, "priority");
    if (cJSON_IsNumber(priority_json))
        priority = priority_json->valueint;
    cJSON *enabled_json = cJSON_GetObjectItem(params, "enabled");
    bool enabled = !cJSON_IsFalse(enabled_json);

    hook_entry_t entry;
    AIRY_MEMSET(&entry, 0, sizeof(entry));
    AIRY_STRNCPY_TERM(entry.name, name, sizeof(entry.name));
    entry.type = (hook_type_t)type;
    entry.impl_type = impl_type;
    if (script_path)
        AIRY_STRNCPY_TERM(entry.script_path, script_path, sizeof(entry.script_path));
    entry.priority = priority;
    entry.enabled = enabled;

    int ret = hook_registry_register(&entry);
    if (ret != 0) {
        const char *msg = ret == -3 ? "Hook name already registered" :
                          ret == -2 ? "Hook registry full" :
                                      "Hook register failed";
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, msg, id);
        SVC_LOG_ERROR("hook.register failed: name=%s error=%d", name, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "registered");
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddStringToObject(result, "type", hook_type_name((hook_type_t)type));
    cJSON_AddBoolToObject(result, "enabled", enabled);
    JSONRPC_SEND_SUCCESS(fd, result, id);
    SVC_LOG_INFO("hook.register OK: name=%s type=%s impl=%s", name,
                 hook_type_name((hook_type_t)type), impl_str);
}

static void handle_unregister(cJSON *params, int id, airy_sock_t fd)
{
    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }

    int ret = hook_registry_unregister(name);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_METHOD_NOT_FOUND, "Hook not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "unregistered");
    cJSON_AddStringToObject(result, "name", name);
    JSONRPC_SEND_SUCCESS(fd, result, id);
    SVC_LOG_INFO("hook.unregister OK: name=%s", name);
}

/* hook.trigger: 按类型触发 Hook 链（hook_service_fire 聚合决策）
 * params: type(字符串或整数), operation(可选), input(可选文本) */
static void handle_trigger(cJSON *params, int id, airy_sock_t fd)
{
    int type = -1;
    cJSON *type_json = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsNumber(type_json)) {
        type = type_json->valueint;
    } else if (cJSON_IsString(type_json)) {
        type = hook_type_from_name(type_json->valuestring);
    }
    if (type < 0 || type >= HOOK_TYPE_COUNT) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Invalid hook type", id);
        return;
    }

    const char *operation = jsonrpc_get_string_param(params, "operation", NULL);
    const char *input = jsonrpc_get_string_param(params, "input", NULL);
    const char *hook_name = jsonrpc_get_string_param(params, "hook_name", NULL);
    const char *session_id = jsonrpc_get_string_param(params, "session_id", NULL);

    hook_context_t ctx;
    AIRY_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.type = (hook_type_t)type;
    ctx.hook_name = hook_name;
    ctx.operation = operation;
    ctx.input_data = input;
    ctx.input_data_len = input ? strlen(input) : 0;
    ctx.timestamp_ns = (uint64_t)time(NULL) * 1000000000ull;
    if (session_id)
        AIRY_STRNCPY_TERM(ctx.session_id, session_id, sizeof(ctx.session_id));

    hook_decision_t decision = hook_service_fire(&ctx);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "decision", (double)decision);
    cJSON_AddStringToObject(result, "decision_name", hook_decision_name(decision));
    cJSON_AddStringToObject(result, "type", hook_type_name((hook_type_t)type));
    JSONRPC_SEND_SUCCESS(fd, result, id);
    SVC_LOG_INFO("hook.trigger OK: type=%s decision=%s", hook_type_name((hook_type_t)type),
                 hook_decision_name(decision));
}

/* hook.session.start: 会话建立时触发 SESSION_START 事件链并注入会话上下文
 * （P1-5 通道）。会话级 hooks 可在此时注入上下文/规则；注入的 input 存入
 * 会话级存储，下游经 hook.session.get 取回。
 * params: session_id(必填), operation(可选), input(可选) */
static void handle_session_start(cJSON *params, int id, airy_sock_t fd)
{
    const char *session_id = jsonrpc_get_string_param(params, "session_id", NULL);
    if (!session_id || !session_id[0]) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing session_id", id);
        return;
    }
    const char *operation = jsonrpc_get_string_param(params, "operation", NULL);
    const char *input = jsonrpc_get_string_param(params, "input", NULL);

    hook_context_t ctx;
    AIRY_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.type = HOOK_TYPE_SESSION_START;
    ctx.operation = operation;
    ctx.input_data = input;
    ctx.input_data_len = input ? strlen(input) : 0;
    ctx.timestamp_ns = (uint64_t)time(NULL) * 1000000000ull;
    AIRY_STRNCPY_TERM(ctx.session_id, session_id, sizeof(ctx.session_id));

    hook_decision_t decision = hook_service_fire(&ctx);

    airy_mtx_lock(&g_hook_sessions_lock);
    hook_session_entry_t *se = hook_session_upsert(session_id);
    if (se) {
        se->hook_count++;
        AIRY_STRNCPY_TERM(se->decision, hook_decision_name(decision), sizeof(se->decision));
        if (input) {
            size_t n = strlen(input);
            if (n >= sizeof(se->context))
                n = sizeof(se->context) - 1;
            AIRY_MEMCPY(se->context, input, n);
            se->context[n] = '\0';
            se->context_len = n;
        }
    }
    airy_mtx_unlock(&g_hook_sessions_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "session_id", session_id);
    cJSON_AddNumberToObject(result, "decision", (double)decision);
    cJSON_AddStringToObject(result, "decision_name", hook_decision_name(decision));
    JSONRPC_SEND_SUCCESS(fd, result, id);
    SVC_LOG_INFO("hook.session.start OK: session=%s decision=%s hooks=%zu", session_id,
                 hook_decision_name(decision), se ? se->hook_count : 0);
}

/* hook.session.get: 取回会话级注入上下文（P1-5 通道读取端）。
 * params: session_id(必填) */
static void handle_session_get(cJSON *params, int id, airy_sock_t fd)
{
    const char *session_id = jsonrpc_get_string_param(params, "session_id", NULL);
    if (!session_id || !session_id[0]) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing session_id", id);
        return;
    }

    airy_mtx_lock(&g_hook_sessions_lock);
    hook_session_entry_t *se = hook_session_find(session_id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "session_id", session_id);
    if (se) {
        cJSON_AddBoolToObject(result, "active", true);
        cJSON_AddNumberToObject(result, "started_at", (double)se->started_ns);
        cJSON_AddStringToObject(result, "decision", se->decision);
        cJSON_AddNumberToObject(result, "hook_count", (double)se->hook_count);
        if (se->context_len > 0)
            cJSON_AddStringToObject(result, "injected_context", se->context);
    } else {
        cJSON_AddBoolToObject(result, "active", false);
    }
    airy_mtx_unlock(&g_hook_sessions_lock);

    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_health_check(int id, airy_sock_t fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "hook_d");
    cJSON_AddBoolToObject(result, "healthy", g_registry_initialized ? true : false);
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}
