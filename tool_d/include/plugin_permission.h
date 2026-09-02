/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plugin_permission.h
 * @brief P2.2.4: plugin permission checks - manifest permissions to
 *        Cupolas guard-type mapping.
 *
 * Maps permissions declared in a plugin manifest to the guard types of the
 * Cupolas security dome. Permissions are validated automatically at plugin
 * load; plugins that do not match the security policy are refused.
 *
 * Permission mapping:
 *   file_read        -> SAFETY_GUARD_FILE_READ
 *   file_write       -> SAFETY_GUARD_FILE_WRITE
 *   network_outbound -> SAFETY_GUARD_NETWORK
 *   tool_execute     -> SAFETY_GUARD_TOOL_EXEC
 *   memory_access    -> SAFETY_GUARD_MEMORY
 *   hook_register    -> SAFETY_GUARD_HOOK
 *   system_call      -> SAFETY_GUARD_SYSTEM
 *   process_spawn    -> SAFETY_GUARD_PROCESS
 */

#ifndef AIRY_RT_PLUGIN_PERMISSION_H
#define AIRY_RT_PLUGIN_PERMISSION_H

#include "plugin_service.h"
#include "safety_guard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    PLUGIN_PERM_ALLOWED = 0,
    PLUGIN_PERM_DENIED = 1,
    PLUGIN_PERM_UNKNOWN = 2,
    PLUGIN_PERM_ERROR = 3,
} plugin_permission_result_t;


typedef struct {
    bool enable_strict_mode;
    bool enable_audit_log;
    char safety_policy_path[512];
    const char *agent_id;
} plugin_permission_config_t;


/**
 * @brief Initialize the permission-check module.
 *
 * @param config Config (NULL = defaults)
 * @return 0 on success, non-zero on failure
 */
int plugin_permission_init(const plugin_permission_config_t *config);

/** @brief Destroy the permission-check module. */
void plugin_permission_destroy(void);

/**
 * @brief Validate a plugin's declared permissions.
 *
 * Maps the manifest's permission declarations to Cupolas guard types and
 * checks each permission against the security policy.
 *
 * @param permissions       Permission-declaration array
 * @param permission_count  Permission count
 * @param plugin_name       Plugin name (for audit)
 * @param out_denied        Output denied permissions (comma-separated)
 * @param out_denied_size   Buffer size
 * @return PLUGIN_PERM_ALLOWED if all pass; otherwise the first denial reason
 */
plugin_permission_result_t plugin_permission_check(const char (*permissions)[64],
                                                   uint32_t permission_count,
                                                   const char *plugin_name, char *out_denied,
                                                   size_t out_denied_size);

/**
 * @brief Map a permission string to a Cupolas guard type.
 *
 * @param permission Permission string
 * @param out_guard  Output guard type
 * @return 0 on success, -1 for unknown permission
 */
int plugin_permission_map_to_guard(const char *permission, safety_guard_type_t *out_guard);

/**
 * @brief Get the human-readable description of a permission.
 *
 * @param permission Permission string
 * @return Description string
 */
const char *plugin_permission_description(const char *permission);

/**
 * @brief Get the list of supported permissions.
 *
 * @param out_permissions Output permission array
 * @param out_count       Output count
 * @return 0 on success
 */
int plugin_permission_list_supported(char ***out_permissions, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_PLUGIN_PERMISSION_H */