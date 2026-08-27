// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_security_acl.c
 * @brief Daemon Layer Security - ACL permission domain.
 *
 * Phase 2.3a split from daemon_security.c: zero-trust permission checks
 * (tool and LLM resources) against the shared ACL table, plus ACL rule
 * management (add/update rules, load rules from a YAML file).
 *
 * All checks are fail-closed: uninitialized module, disabled permission
 * subsystem or missing ACL entry denies access (K-4, E-1).
 *
 * The public API surface (daemon_security.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/daemon_security.c (init/sanitize domain)
 * @see agentrt/daemons/common/src/daemon_security_internal.h
 */

#include "daemon_security_internal.h"

#include "error.h"
#include "yaml_minimal.h"

#include <stdio.h>
#include <string.h>

int daemon_check_tool_permission(const char *agent_id, const char *tool_name, const char *action)
{
    if (!agent_id || !tool_name || !action) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_check_tool_permission: daemon_security not initialized — "
                      "call daemon_cupolas_init() during startup. DENYING %s/%s (fail-closed).",
                      agent_id, tool_name);
        return AIRY_EPERM;
    }

    if (!g_security_ctx.permission_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("Permission check: disabled by configuration, DENYING %s/%s (fail-closed)",
                     agent_id, tool_name);
        return AIRY_EPERM;
    }

    int result = AIRY_ERR_PERMISSION_DENIED;
    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strcmp(g_security_ctx.acl_table[i].resource, tool_name) == 0) {

            if (g_security_ctx.acl_table[i].allowed) {
                result = AIRY_OK;
            }
            break;
        }
    }
    airy_mtx_unlock(&g_security_mutex);

    if (result == AIRY_OK) {
        SVC_LOG_DEBUG("Permission GRANTED: agent=%s tool=%s action=%s", agent_id, tool_name,
                      action);
    } else {
        SVC_LOG_SECURITY("Permission DENIED (no ACL entry): agent=%s tool=%s action=%s", agent_id,
                         tool_name, action);
    }
    return result;
}

int daemon_check_llm_permission(const char *agent_id, const char *model_name, const char *action)
{
    if (!agent_id || !model_name || !action) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_check_llm_permission: daemon_security not initialized — "
                      "call daemon_cupolas_init() during startup. DENYING %s/%s (fail-closed).",
                      agent_id, model_name);
        return AIRY_EPERM;
    }

    if (!g_security_ctx.permission_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("LLM permission check: disabled by configuration, DENYING %s/%s (fail-closed)",
                     agent_id, model_name);
        return AIRY_EPERM;
    }

    char resource[256];
    snprintf(resource, sizeof(resource), "llm:%s", model_name);

    int result = AIRY_ERR_PERMISSION_DENIED;
    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strstr(g_security_ctx.acl_table[i].resource, resource) != NULL) {

            if (g_security_ctx.acl_table[i].allowed) {
                result = AIRY_OK;
            }
            break;
        }
    }
    airy_mtx_unlock(&g_security_mutex);

    if (result == AIRY_OK) {
        SVC_LOG_DEBUG("LLM Permission GRANTED: agent=%s model=%s action=%s", agent_id, model_name,
                      action);
    } else {
        SVC_LOG_SECURITY("LLM Permission DENIED (no ACL): agent=%s model=%s action=%s", agent_id,
                         model_name, action);
    }
    return result;
}

int daemon_security_add_acl_rule(const char *agent_id, const char *resource, bool allowed)
{
    if (!agent_id || !resource || agent_id[0] == '\0' || resource[0] == '\0') {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (strlen(agent_id) >= sizeof(g_security_ctx.acl_table[0].agent_id) ||
        strlen(resource) >= sizeof(g_security_ctx.acl_table[0].resource)) {
        SVC_LOG_ERROR("ACL rule rejected: agent_id or resource too long (agent=%s resource=%s)",
                      agent_id, resource);
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR(
            "daemon_security_add_acl_rule: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. DENYING ACL rule add (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strcmp(g_security_ctx.acl_table[i].resource, resource) == 0) {
            g_security_ctx.acl_table[i].allowed = allowed;
            airy_mtx_unlock(&g_security_mutex);
            SVC_LOG_DEBUG("ACL rule updated: agent=%s resource=%s allowed=%d", agent_id, resource,
                          allowed ? 1 : 0);
            return AIRY_OK;
        }
    }

    if (g_security_ctx.acl_count >= MAX_ACL_ENTRIES) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("ACL table full (max=%d), cannot add rule for agent=%s resource=%s",
                      MAX_ACL_ENTRIES, agent_id, resource);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    acl_entry_t *entry = &g_security_ctx.acl_table[g_security_ctx.acl_count];
    snprintf(entry->agent_id, sizeof(entry->agent_id), "%s", agent_id);
    snprintf(entry->resource, sizeof(entry->resource), "%s", resource);
    entry->operations = 0xFFFFFFFF;
    entry->allowed = allowed;
    g_security_ctx.acl_count++;
    airy_mtx_unlock(&g_security_mutex);

    SVC_LOG_INFO("ACL rule added: agent=%s resource=%s allowed=%d (count=%zu/%d)", agent_id,
                 resource, allowed ? 1 : 0, g_security_ctx.acl_count, MAX_ACL_ENTRIES);
    return AIRY_OK;
}

int daemon_security_load_rules_file(const char *path)
{
    if (!path || !path[0])
        return AIRY_ERR_INVALID_PARAM;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return AIRY_ERR_NOT_FOUND;
    fclose(fp);

    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_ERR_OUT_OF_MEMORY;
    if (yaml_parse_file(doc, path) != 0) {
        SVC_LOG_ERROR("daemon_security: failed to parse permission rules file: %s (%s)", path,
                      yaml_get_error(doc) ? yaml_get_error(doc) : "parse error");
        yaml_destroy(doc);
        return AIRY_ERR_PARSE_ERROR;
    }

    struct yaml_node *root = yaml_root(doc);
    struct yaml_node *rules = root ? yaml_get(root, "rules") : NULL;
    if (!rules) {
        SVC_LOG_WARN("daemon_security: permission rules file has no 'rules' section: %s", path);
        yaml_destroy(doc);
        return AIRY_ERR_PARSE_ERROR;
    }

    size_t total = yaml_size(rules);
    size_t added = 0;
    for (size_t i = 0; i < total; i++) {
        struct yaml_node *r = yaml_get_index(rules, i);
        if (!r)
            continue;
        const char *agent = yaml_as_string(yaml_get(r, "agent"), NULL);
        const char *tool = yaml_as_string(yaml_get(r, "tool"), NULL);
        const char *effect = yaml_as_string(yaml_get(r, "effect"), "deny");
        if (!agent || !tool || !effect || agent[0] == '\0' || tool[0] == '\0')
            continue;
        bool allowed = (strcmp(effect, "allow") == 0);
        if (daemon_security_add_acl_rule(agent, tool, allowed) == AIRY_OK)
            added++;
    }
    yaml_destroy(doc);

    SVC_LOG_INFO("daemon_security: loaded %zu/%zu permission rules from %s", added, total, path);
    return AIRY_OK;
}
