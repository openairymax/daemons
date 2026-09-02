// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plugin_rpc.c
 * @brief 插件执行域 RPC 服务（0.1.9 M4：plugin_d → tool_d 整编）。
 *
 * plugin_d（dlopen 插件执行域）并入 tool_d（工具注册表与执行域）：
 * plugin.load/unload/start/stop/execute/get_metadata/get_state/get_stats/
 * list/install/uninstall/health_check 方法在 tool.* 命名空间登记；
 * gateway 旧 plugin.* cap 改路由到 tool 命名空间（§5 旧 namespace 保留
 * 转发）。dlopen 引擎本体（plugin_service/discovery/permission）随迁
 * tool_d/src，生命周期由本模块承载。
 *
 * 插件扫描目录：$AIRY_HOME/ecosystem/plugins（绝对路径，随进程 CWD 漂移
 * 的历史根因已修复）；扫描结果权限校验（fail-closed）后加载。
 */

#include "airy_memory.h"
#include "daemon_main.h"
#include "error.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "platform.h"
#include "plugin_discovery.h"
#include "plugin_permission.h"
#include "plugin_service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

static plugin_discovery_result_t *g_discovered = NULL;
static size_t g_discovered_count = 0;

static void prpc_scan_load(void)
{
    if (plugin_discovery_scan(&g_discovered, &g_discovered_count) != 0) {
        SVC_LOG_WARN("plugin_rpc: discovery scan failed");
        return;
    }
    if (g_discovered_count == 0) {
        SVC_LOG_INFO("plugin_rpc: no plugins found");
        return;
    }
    SVC_LOG_INFO("plugin_rpc: found %zu plugin(s)", g_discovered_count);
    size_t loaded = 0;
    for (size_t i = 0; i < g_discovered_count; i++) {
        if (!g_discovered[i].valid) {
            SVC_LOG_WARN("plugin_rpc: '%s' invalid: %s", g_discovered[i].name,
                         g_discovered[i].error_reason);
            continue;
        }
        char denied[512] = {0};
        plugin_permission_result_t perm_result =
            plugin_permission_check((const char(*)[64])g_discovered[i].permissions,
                                    g_discovered[i].permission_count, g_discovered[i].name,
                                    denied, sizeof(denied));
        if (perm_result != PLUGIN_PERM_ALLOWED) {
            SVC_LOG_WARN("plugin_rpc: skipping '%s' — permission denied: %s",
                         g_discovered[i].name, denied);
            continue;
        }
        const char *out_name = NULL;
        if (plugin_service_load(g_discovered[i].library_path, NULL, &out_name) == 0) {
            plugin_service_start(g_discovered[i].name);
            loaded++;
            SVC_LOG_INFO("plugin_rpc: loaded and started '%s'", g_discovered[i].name);
        } else {
            SVC_LOG_ERROR("plugin_rpc: failed to load '%s' from '%s'",
                          g_discovered[i].name, g_discovered[i].library_path);
        }
    }
    SVC_LOG_INFO("plugin_rpc: loaded %zu/%zu plugin(s)", loaded, g_discovered_count);
}

int plugin_rpc_init(void)
{
    plugin_permission_config_t perm_cfg;
    __builtin_memset(&perm_cfg, 0, sizeof(perm_cfg));
    perm_cfg.enable_strict_mode = true;
    perm_cfg.enable_audit_log = true;
    perm_cfg.agent_id = "tool_d";
    if (plugin_permission_init(&perm_cfg) != 0)
        SVC_LOG_WARN("plugin_rpc: permission module init failed");

    char plugins_dir[AIRY_PATH_MAX];
    snprintf(plugins_dir, sizeof(plugins_dir), "%s/ecosystem/plugins", airy_home_dir());
    plugin_discovery_config_t disc_cfg;
    __builtin_memset(&disc_cfg, 0, sizeof(disc_cfg));
    disc_cfg.plugins_dir = plugins_dir;
    disc_cfg.auto_load = false;
    disc_cfg.fail_on_invalid = false;
    disc_cfg.scan_depth = 1;
    if (plugin_discovery_init(&disc_cfg) != 0)
        SVC_LOG_ERROR("plugin_rpc: plugin discovery init failed");

    prpc_scan_load();
    return 0;
}

void plugin_rpc_cleanup(void)
{
    plugin_discovery_free_results(g_discovered, g_discovered_count);
    g_discovered = NULL;
    g_discovered_count = 0;
    plugin_discovery_destroy();
    plugin_permission_destroy();
}

/* ── RPC 处理器（自 plugin_d main.c 移植，语义不变） ───────────────── */
static void prpc_handle_load(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *library_path = cJSON_GetObjectItem(params, "library_path");
    cJSON *config_path = cJSON_GetObjectItem(params, "config_path");
    if (!library_path || !cJSON_IsString(library_path)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing library_path string", id);
        return;
    }
    const char *out_name = NULL;
    int ret = plugin_service_load(library_path->valuestring,
                                  config_path && cJSON_IsString(config_path)
                                      ? config_path->valuestring
                                      : NULL,
                                  &out_name);
    if (ret != 0 || !out_name) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin load failed", id);
        SVC_LOG_ERROR("plugin.load failed: library=%s error=%d", library_path->valuestring, ret);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", out_name);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.load OK: name=%s library=%s", out_name, library_path->valuestring);
}

static void prpc_handle_unload(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }
    int ret = plugin_service_unload(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin unload failed", id);
        SVC_LOG_ERROR("plugin.unload failed: name=%s error=%d", name->valuestring, ret);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "unloaded", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_start(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }
    int ret = plugin_service_start(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin start failed", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "started", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_stop(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }
    int ret = plugin_service_stop(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin stop failed", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "stopped", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_exec(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    cJSON *input = cJSON_GetObjectItem(params, "input");
    if (!name || !cJSON_IsString(name) || !input || !cJSON_IsString(input)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name/input strings", id);
        return;
    }
    char *output = NULL;
    int ret = plugin_service_execute(name->valuestring, input->valuestring, &output);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin execute failed", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "output", output ? output : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(output);
}

static void prpc_handle_metadata(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }
    plugin_metadata_t metadata;
    __builtin_memset(&metadata, 0, sizeof(metadata));
    if (plugin_service_get_metadata(name->valuestring, &metadata) != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Plugin not found", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", metadata.name);
    cJSON_AddStringToObject(result, "version", metadata.version);
    cJSON_AddStringToObject(result, "author", metadata.author);
    cJSON_AddStringToObject(result, "description", metadata.description);
    cJSON_AddNumberToObject(result, "type", (double)metadata.type);
    cJSON_AddNumberToObject(result, "api_version", (double)metadata.api_version);
    cJSON_AddNumberToObject(result, "min_airy_version", (double)metadata.min_airy_version);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_state(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "state", (double)plugin_service_get_state(name->valuestring));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_stats(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        size_t count = 0;
        uint64_t loads = 0, errors = 0, memory = 0;
        if (plugin_service_get_daemon_stats(&count, &loads, &errors, &memory) != 0) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Aggregate stats failed", id);
            return;
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "daemon", "tool_d");
        cJSON_AddNumberToObject(result, "plugins", (double)count);
        cJSON_AddNumberToObject(result, "load_total", (double)loads);
        cJSON_AddNumberToObject(result, "error_total", (double)errors);
        cJSON_AddNumberToObject(result, "memory_bytes", (double)memory);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        return;
    }
    plugin_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    if (plugin_service_get_stats(name->valuestring, &stats) != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Plugin not found", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "load_count", (double)stats.load_count);
    cJSON_AddNumberToObject(result, "error_count", (double)stats.error_count);
    cJSON_AddNumberToObject(result, "uptime_ns", (double)stats.uptime_ns);
    cJSON_AddNumberToObject(result, "memory_bytes", (double)stats.memory_bytes);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void prpc_handle_list(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *type_filter = cJSON_GetObjectItem(params, "type_filter");
    int filter = type_filter && cJSON_IsNumber(type_filter) ? type_filter->valueint : -1;
    char **names = NULL;
    size_t count = 0;
    if (plugin_service_list(&names, &count, filter) != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Plugin list failed", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        if (names[i])
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    cJSON_AddItemToObject(result, "plugins", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    if (names) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(names[i]);
        AIRY_FREE(names);
    }
}

static void prpc_handle_health(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    (void)params;
    bool healthy = true;
    size_t plugin_count = 0;
    char **names = NULL;
    if (plugin_service_list(&names, &plugin_count, -1) != 0)
        healthy = false;
    if (names) {
        for (size_t i = 0; i < plugin_count; i++)
            AIRY_FREE(names[i]);
        AIRY_FREE(names);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "tool_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddNumberToObject(result, "plugin_count", (double)plugin_count);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

void plugin_rpc_register(void *disp)
{
    method_dispatcher_t *d = (method_dispatcher_t *)disp;
    if (!d)
        return;
    /* plugin_* 前缀登记：与 tool.execute/list 等既有方法名区分，避免
     * 覆盖 tool 语义（tool.execute=工具执行，plugin_execute=插件执行）。 */
    method_dispatcher_register(d, "plugin_load", prpc_handle_load, NULL);
    method_dispatcher_register(d, "plugin_unload", prpc_handle_unload, NULL);
    method_dispatcher_register(d, "plugin_start", prpc_handle_start, NULL);
    method_dispatcher_register(d, "plugin_stop", prpc_handle_stop, NULL);
    method_dispatcher_register(d, "plugin_execute", prpc_handle_exec, NULL);
    method_dispatcher_register(d, "plugin_get_metadata", prpc_handle_metadata, NULL);
    method_dispatcher_register(d, "plugin_get_state", prpc_handle_state, NULL);
    method_dispatcher_register(d, "plugin_get_stats", prpc_handle_stats, NULL);
    method_dispatcher_register(d, "plugin_list", prpc_handle_list, NULL);
    /* L2 协议标准方法 + 别名（plugin.install / uninstall / health_check） */
    method_dispatcher_register(d, "plugin_install", prpc_handle_load, NULL);
    method_dispatcher_register(d, "plugin_uninstall", prpc_handle_unload, NULL);
    method_dispatcher_register(d, "plugin_health_check", prpc_handle_health, NULL);
    SVC_LOG_INFO("plugin_rpc: registered plugin.* methods on tool_d dispatcher "
                 "(plugin_load/unload/start/stop/execute/metadata/state/stats/list/"
                 "install/uninstall/health_check)");
}
