/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file plugin_discovery.h
 * @brief P2.2.1: plugin discovery - scan directories + parse manifest.yaml.
 *
 * Scans the ecosystem/plugins/ directory to discover all plugins. Each
 * plugin directory must contain a manifest.yaml description file.
 *
 * manifest.yaml format:
 * @code
 *   name: my_plugin
 *   version: 1.0.0
 *   author: SPHARX
 *   description: My plugin description
 *   type: tool_provider
 *   api_version: 1
 *   min_airy_version: 0.1.1
 *   library: libmy_plugin.so
 *   permissions:
 *     - file_read
 *     - network_outbound
 *     - tool_execute
 *   config:
 *     timeout_ms: 5000
 * @endcode
 */

#ifndef AIRY_RT_PLUGIN_DISCOVERY_H
#define AIRY_RT_PLUGIN_DISCOVERY_H

#include "plugin_service.h"

#include "airy_abi.h"

AIRY_ABI_BEGIN


#define PLUGIN_DISCOVERY_MAX_PLUGINS 128
#define PLUGIN_DISCOVERY_MAX_PATH 512
#define PLUGIN_DISCOVERY_MAX_PERMISSIONS 32

typedef struct {
    char name[64];
    char version[32];
    char author[64];
    char description[256];
    plugin_type_t type;
    uint32_t api_version;
    uint32_t min_airy_version;
    char library_path[PLUGIN_DISCOVERY_MAX_PATH];
    char config_path[PLUGIN_DISCOVERY_MAX_PATH];
    char permissions[PLUGIN_DISCOVERY_MAX_PERMISSIONS][64];
    uint32_t permission_count;
    bool valid;
    char error_reason[256];
} plugin_discovery_result_t;


typedef struct {
    const char *plugins_dir;
    bool fail_on_invalid;
} plugin_discovery_config_t;


/**
 * @brief Initialize the plugin-discovery module.
 *
 * @param config Config (NULL = defaults)
 * @return 0 on success, non-zero on failure
 */
int plugin_discovery_init(const plugin_discovery_config_t *config);

/** @brief Destroy the plugin-discovery module. */
void plugin_discovery_destroy(void);

/**
 * @brief Scan the plugin directory and discover all available plugins.
 *
 * Scans every subdirectory under plugins_dir looking for manifest.yaml,
 * and returns the metadata of all discovered plugins.
 *
 * @param out_results Output discovered-result array (caller frees)
 * @param out_count   Output count
 * @return 0 on success, non-zero on failure
 */
int plugin_discovery_scan(plugin_discovery_result_t **out_results, size_t *out_count);

/**
 * @brief Free the discovery results.
 *
 * @param results Result array
 * @param count   Count
 */
void plugin_discovery_free_results(plugin_discovery_result_t *results, size_t count);

AIRY_ABI_END

#endif /* AIRY_RT_PLUGIN_DISCOVERY_H */
