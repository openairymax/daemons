// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plugin_permission.c
 * @brief P2.2.4: plugin permission checks - manifest permissions to
 *        Cupolas guard-type mapping.
 *
 * Maps permissions declared in a plugin manifest to the guard types of the
 * Cupolas security dome. Permissions are validated automatically at plugin
 * load; plugins that do not match the security policy are refused.
 */

#include "plugin_permission.h"
#include "safety_guard.h"
#include "safe_string_utils.h"

#include "logger.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 权限元数据单表（SSoT）：声明名、Cupolas guard 映射与人类可读描述
 * 三份平行数据合一，新增权限只改一处。 */
static const struct {
    const char *name;
    safety_guard_type_t guard;
    const char *desc;
} PERM_META[] = {
    {"file_read", SAFETY_GUARD_FILE_READ, "Read files from the filesystem"},
    {"file_write", SAFETY_GUARD_FILE_WRITE, "Write files to the filesystem"},
    {"network_outbound", SAFETY_GUARD_NETWORK, "Make outbound network connections"},
    {"network_inbound", SAFETY_GUARD_NETWORK, "Accept inbound network connections"},
    {"tool_execute", SAFETY_GUARD_TOOL_EXEC, "Execute tools via tool_d"},
    {"memory_access", SAFETY_GUARD_MEMORY, "Access agent memory via memory_d"},
    {"hook_register", SAFETY_GUARD_HOOK, "Register hooks in notify_d"},
    {"system_call", SAFETY_GUARD_SYSTEM, "Make system calls"},
    {"process_spawn", SAFETY_GUARD_PROCESS, "Spawn child processes"},
    {"ipc_connect", SAFETY_GUARD_IPC, "Connect to IPC bus"},
    {"service_discovery", SAFETY_GUARD_SERVICE_DISCOVERY, "Use service discovery"},
    {"config_read", SAFETY_GUARD_CONFIG, "Read configuration"},
    {"config_write", SAFETY_GUARD_CONFIG, "Write configuration"},
    {"log_write", SAFETY_GUARD_LOGGING, "Write log entries"},
    {"metrics_export", SAFETY_GUARD_METRICS, "Export metrics"},
    {"audit_trigger", SAFETY_GUARD_AUDIT, "Trigger audit events"},
};

#define PERM_META_COUNT (sizeof(PERM_META) / sizeof(PERM_META[0]))

static struct {
    plugin_permission_config_t config;
    safety_guard_context_t *guard_ctx;
    bool initialized;
} g_permission;

int plugin_permission_init(const plugin_permission_config_t *config)
{
    if (g_permission.initialized)
        return 0;

    __builtin_memset(&g_permission, 0, sizeof(g_permission));

    if (config) {
        g_permission.config = *config;
    } else {
        g_permission.config.enable_strict_mode = true;
        g_permission.config.enable_audit_log = true;
        g_permission.config.safety_policy_path[0] = '\0';
        g_permission.config.agent_id = "tool_d";
    }

    g_permission.guard_ctx = safety_guard_create();
    if (!g_permission.guard_ctx) {
        AIRY_LOG_WARN("PluginPermission: safety_guard_create failed, "
                      "permission checks will be local-only");
    }

    g_permission.initialized = true;

    AIRY_LOG_INFO("PluginPermission: initialized (strict=%d, audit=%d)",
                  g_permission.config.enable_strict_mode, g_permission.config.enable_audit_log);
    return 0;
}

void plugin_permission_destroy(void)
{
    if (g_permission.guard_ctx) {
        safety_guard_destroy(g_permission.guard_ctx);
        g_permission.guard_ctx = NULL;
    }

    AIRY_LOG_INFO("PluginPermission: destroyed");
    g_permission.initialized = false;
}

int plugin_permission_map_to_guard(const char *permission, safety_guard_type_t *out_guard)
{
    if (!permission || !out_guard)
        return AIRY_ERR_INVALID_PARAM;

    for (size_t i = 0; i < PERM_META_COUNT; i++) {
        if (strcmp(permission, PERM_META[i].name) == 0) {
            *out_guard = PERM_META[i].guard;
            return 0;
        }
    }

    AIRY_LOG_WARN("PluginPermission: unknown permission '%s'", permission);
    return AIRY_ERR_NOT_SUPPORTED;
}

plugin_permission_result_t plugin_permission_check(const char (*permissions)[64],
                                                   uint32_t permission_count,
                                                   const char *plugin_name, char *out_denied,
                                                   size_t out_denied_size)
{
    if (!permissions || permission_count == 0) {

        if (g_permission.config.enable_strict_mode) {
            if (out_denied && out_denied_size > 0) {
                safe_strcpy(out_denied, "no permissions declared", out_denied_size);
            }
            AIRY_LOG_WARN("PluginPermission: plugin '%s' has no permissions "
                          "(strict mode)",
                          plugin_name);
            return PLUGIN_PERM_DENIED;
        }
        return PLUGIN_PERM_ALLOWED;
    }

    if (!g_permission.initialized) {
        plugin_permission_init(NULL);
    }

    plugin_permission_result_t overall = PLUGIN_PERM_ALLOWED;
    char denied_list[512] = {0};
    size_t denied_offset = 0;

    for (uint32_t i = 0; i < permission_count; i++) {
        const char *perm = permissions[i];
        if (!perm || perm[0] == '\0')
            continue;

        safety_guard_type_t guard_type;
        int map_ret = plugin_permission_map_to_guard(perm, &guard_type);

        if (map_ret != 0) {

            AIRY_LOG_WARN("PluginPermission: plugin '%s' declares unknown "
                          "permission '%s'",
                          plugin_name, perm);
            if (g_permission.config.enable_strict_mode) {
                overall = PLUGIN_PERM_UNKNOWN;
                if (denied_offset < sizeof(denied_list) - 1) {
                    denied_offset += (size_t)snprintf(denied_list + denied_offset,
                                                      sizeof(denied_list) - denied_offset, "%s%s",
                                                      denied_offset > 0 ? "," : "", perm);
                }
            }
            continue;
        }

        if (g_permission.guard_ctx) {
            bool allowed = false;
            int check_ret = safety_guard_check_permission(g_permission.guard_ctx, guard_type,
                                                          g_permission.config.agent_id ?
                                                              g_permission.config.agent_id :
                                                              "tool_d",
                                                          &allowed);

            if (check_ret != 0 || !allowed) {
                AIRY_LOG_WARN("PluginPermission: permission '%s' denied "
                              "for plugin '%s'",
                              perm, plugin_name);
                overall = PLUGIN_PERM_DENIED;
                if (denied_offset < sizeof(denied_list) - 1) {
                    denied_offset += (size_t)snprintf(denied_list + denied_offset,
                                                      sizeof(denied_list) - denied_offset, "%s%s",
                                                      denied_offset > 0 ? "," : "", perm);
                }
            } else {
                AIRY_LOG_DEBUG("PluginPermission: permission '%s' allowed "
                               "for plugin '%s'",
                               perm, plugin_name);
            }
        }
    }

    if (out_denied && out_denied_size > 0) {
        safe_strcpy(out_denied, denied_list, out_denied_size);
    }

    if (overall != PLUGIN_PERM_ALLOWED && g_permission.config.enable_audit_log) {
        AIRY_LOG_INFO("PluginPermission: AUDIT plugin='%s' result=%d "
                      "denied='%s'",
                      plugin_name, overall, denied_list);
    }

    return overall;
}
