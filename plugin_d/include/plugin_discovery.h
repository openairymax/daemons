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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


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
    bool auto_load;
    bool fail_on_invalid;
    uint32_t scan_depth;
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
 * @brief Parse a single manifest.yaml file.
 *
 * @param yaml_path   manifest.yaml file path
 * @param plugin_dir  Plugin directory (for resolving relative paths)
 * @param out_result  Output parse result
 * @return 0 on success, non-zero on failure
 */
int plugin_discovery_parse_manifest(const char *yaml_path, const char *plugin_dir,
                                    plugin_discovery_result_t *out_result);

/**
 * @brief Auto-load all discovered plugins.
 *
 * Calls plugin_discovery_scan() to discover plugins, then
 * plugin_service_load() for every valid plugin.
 *
 * @return 0 on success, non-zero on failure
 */
int plugin_discovery_auto_load(void);

/**
 * @brief 离线校验插件目录（P1-5 扩展校验器）。
 *
 * 装前/加载前校验（fail-closed）：目录存在、manifest.yaml 存在且
 * schema 合法（name/type/library/api_version/min_airy_version 必填、
 * type 为合法枚举）、库文件存在、权限声明非空。任一项不满足即
 * invalid，绝不部分接受。
 *
 * @param plugin_dir 插件目录绝对路径（含 manifest.yaml）
 * @param out_result 校验结果（valid + error_reason；可 NULL 仅跑校验）
 * @return 0 且 out_result->valid=true 通过；非 0 失败（valid=false）
 */
int plugin_discovery_validate_plugin(const char *plugin_dir, plugin_discovery_result_t *out_result);

/**
 * @brief Get the number of discovered plugins.
 *
 * @return Plugin count
 */
size_t plugin_discovery_count(void);

/**
 * @brief Free the discovery results.
 *
 * @param results Result array
 * @param count   Count
 */
void plugin_discovery_free_results(plugin_discovery_result_t *results, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_PLUGIN_DISCOVERY_H */