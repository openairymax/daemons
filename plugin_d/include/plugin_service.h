/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plugin_service.h
 * @brief Plugin daemon service interface.
 *
 * The Plugin daemon manages dynamic-plugin loading, unloading, lifecycle
 * and sandboxing. Supported plugin types:
 *   - TOOL_PROVIDER:     tool-provider plugin
 *   - PROTOCOL_ADAPTER:  protocol-adapter plugin
 *   - MEMORY_PROVIDER:   memory-provider plugin
 *   - HOOK_EXTENSION:    hook-extension plugin
 *
 * @owner team-A
 * @see contracts/contract_A_B.h section 3 (protocol-adapter vtable)
 */

#ifndef AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H
#define AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    PLUGIN_TYPE_TOOL_PROVIDER = 0,
    PLUGIN_TYPE_PROTOCOL_ADAPTER = 1,
    PLUGIN_TYPE_MEMORY_PROVIDER = 2,
    PLUGIN_TYPE_HOOK_EXTENSION = 3,
    PLUGIN_TYPE_COUNT = 4
} plugin_type_t;


typedef enum {
    PLUGIN_STATE_UNLOADED = 0,
    PLUGIN_STATE_LOADED = 1,
    PLUGIN_STATE_INITIALIZED = 2,
    PLUGIN_STATE_RUNNING = 3,
    PLUGIN_STATE_ERROR = 4,
    PLUGIN_STATE_DISABLED = 5,
    PLUGIN_STATE_STARTING = 6
} plugin_state_t;


typedef struct {
    char name[64];
    char version[32];
    char author[64];
    char description[256];
    plugin_type_t type;
    uint32_t api_version;
    uint32_t min_airy_version;
} plugin_metadata_t;


/**
 * @brief Plugin-init callback.
 * @param config_path Config file path
 * @param user_data   User-data output
 * @return 0 on success, non-zero on failure
 */
typedef int (*plugin_init_fn)(const char *config_path, void **user_data);

/**
 * @brief Plugin-destroy callback.
 * @param user_data User data
 */
typedef void (*plugin_destroy_fn)(void *user_data);

/**
 * @brief Plugin-start callback.
 * @param user_data User data
 * @return 0 on success, non-zero on failure
 */
typedef int (*plugin_start_fn)(void *user_data);

/**
 * @brief Plugin-stop callback.
 * @param user_data User data
 * @return 0 on success, non-zero on failure
 */
typedef int (*plugin_stop_fn)(void *user_data);


typedef struct {
    plugin_metadata_t metadata;
    plugin_init_fn init;
    plugin_destroy_fn destroy;
    plugin_start_fn start;
    plugin_stop_fn stop;
    void *handle;
    void *user_data;
    plugin_state_t state;
    char config_path[256];
    char library_path[256];
} plugin_descriptor_t;


typedef struct {
    uint64_t load_count;
    uint64_t error_count;
    uint64_t uptime_ns;
    uint64_t memory_bytes;
} plugin_stats_t;


/**
 * @brief Load a plugin from a dynamic library.
 * @param library_path Dynamic-library path
 * @param config_path  Config file path
 * @param out_name     Output plugin name
 * @return 0 on success, non-zero on failure
 */
int plugin_service_load(const char *library_path, const char *config_path, const char **out_name);

/**
 * @brief Unload a plugin.
 * @param name Plugin name
 * @return 0 on success, non-zero on failure
 */
int plugin_service_unload(const char *name);

/**
 * @brief Start a plugin.
 * @param name Plugin name
 * @return 0 on success, non-zero on failure
 */
int plugin_service_start(const char *name);

/**
 * @brief Stop a plugin.
 * @param name Plugin name
 * @return 0 on success, non-zero on failure
 */
int plugin_service_stop(const char *name);

/**
 * @brief Get plugin metadata.
 * @param name     Plugin name
 * @param metadata Output metadata
 * @return 0 on success, non-zero on failure
 */
int plugin_service_get_metadata(const char *name, plugin_metadata_t *metadata);

/**
 * @brief Get the plugin state.
 * @param name  Plugin name
 * @return Plugin state
 */
plugin_state_t plugin_service_get_state(const char *name);

/**
 * @brief Get plugin statistics.
 * @param name  Plugin name
 * @param stats Output statistics
 * @return 0 on success, non-zero on failure
 */
int plugin_service_get_stats(const char *name, plugin_stats_t *stats);

/**
 * @brief List all loaded plugins.
 * @param names       Output name array (caller frees)
 * @param count       Output count
 * @param type_filter Type filter (-1 = all types)
 * @return 0 on success, non-zero on failure
 */
int plugin_service_list(char ***names, size_t *count, int type_filter);

/**
 * @brief Execute a plugin (calls the plugin_execute symbol it exports).
 *
 * Skill-type plugins expose a JSON-input -> JSON-output execution entry
 * via plugin_execute, matching the airy_sys_skill_execute() contract in
 * the design docs. plugin_d resolves the optional plugin_execute symbol
 * via dlsym; executing a plugin that does not export it returns
 * AIRY_ERR_NOT_FOUND.
 *
 * @param name        Plugin name
 * @param json_input  Input JSON string
 * @param json_output Output JSON string (allocated by the plugin, caller AIRY_FREEs)
 * @return 0 on success, non-zero on failure
 */
int plugin_service_execute(const char *name, const char *json_input, char **json_output);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_PLUGIN_D_PLUGIN_SERVICE_H */
