// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plugin_discovery.c
 * @brief P2.2.1: 插件发现实现 — 扫描目录 + 解析 manifest.yaml
 *
 * 扫描 ecosystem/plugins/ 目录下的每个子目录，
 * 查找并解析 manifest.yaml，提取插件元数据。
 */

#include "plugin_discovery.h"
#include "plugin_service.h"
#include "safe_string_utils.h"

#include "logger.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <dirent.h>
#else
#include <windows.h>
#endif

#define DEFAULT_PLUGINS_DIR "ecosystem/plugins/"
#define DEFAULT_SCAN_DEPTH 1

static struct {
    plugin_discovery_config_t config;
    char plugins_dir[PLUGIN_DISCOVERY_MAX_PATH];
    plugin_discovery_result_t *results;
    size_t count;
    bool initialized;
} g_discovery;

/**
 * @brief Simple YAML key-value parsing (no third-party dependency)
 *
 * Only parses the simple key-value pairs in manifest.yaml.
 * Format: key: value
 */
static char *parse_yaml_value(const char *line, const char *key)
{
    if (!line || !key)
        return NULL;

    while (*line == ' ' || *line == '\t')
        line++;

    size_t key_len = strlen(key);
    if (strncmp(line, key, key_len) != 0)
        return NULL;

    const char *rest = line + key_len;

    while (*rest == ' ' || *rest == ':')
        rest++;

    size_t val_len = strlen(rest);
    while (val_len > 0 &&
           (rest[val_len - 1] == '\n' || rest[val_len - 1] == '\r' || rest[val_len - 1] == ' ')) {
        val_len--;
    }

    if (val_len == 0)
        return NULL;

    char *result = (char *)AIRY_MALLOC(val_len + 1);
    if (!result)
        return NULL;

    AIRY_MEMCPY(result, rest, val_len);
    result[val_len] = '\0';
    return result;
}

/**
 * @brief Parse the plugin type string
 */
static plugin_type_t parse_plugin_type(const char *type_str)
{
    if (!type_str)
        return PLUGIN_TYPE_TOOL_PROVIDER;

    if (strcmp(type_str, "tool_provider") == 0)
        return PLUGIN_TYPE_TOOL_PROVIDER;
    if (strcmp(type_str, "protocol_adapter") == 0)
        return PLUGIN_TYPE_PROTOCOL_ADAPTER;
    if (strcmp(type_str, "memory_provider") == 0)
        return PLUGIN_TYPE_MEMORY_PROVIDER;
    if (strcmp(type_str, "hook_extension") == 0)
        return PLUGIN_TYPE_HOOK_EXTENSION;

    return PLUGIN_TYPE_TOOL_PROVIDER;
}

/**
 * @brief Check whether the directory exists
 */
static bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @brief Check whether the file exists
 */
static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int plugin_discovery_init(const plugin_discovery_config_t *config)
{
    if (g_discovery.initialized)
        return 0;

    __builtin_memset(&g_discovery, 0, sizeof(g_discovery));

    if (config) {
        g_discovery.config = *config;
        if (config->plugins_dir) {
            safe_strcpy(g_discovery.plugins_dir, config->plugins_dir,
                        sizeof(g_discovery.plugins_dir));
        }
    } else {
        g_discovery.config.auto_load = false;
        g_discovery.config.fail_on_invalid = false;
        g_discovery.config.scan_depth = DEFAULT_SCAN_DEPTH;
    }

    if (g_discovery.plugins_dir[0] == '\0') {
        safe_strcpy(g_discovery.plugins_dir, DEFAULT_PLUGINS_DIR, sizeof(g_discovery.plugins_dir));
    }

    g_discovery.initialized = true;

    AIRY_LOG_INFO("PluginDiscovery: initialized (dir=%s, auto_load=%d, "
                  "scan_depth=%u)",
                  g_discovery.plugins_dir, g_discovery.config.auto_load,
                  g_discovery.config.scan_depth);
    return 0;
}

void plugin_discovery_destroy(void)
{
    if (g_discovery.results) {
        plugin_discovery_free_results(g_discovery.results, g_discovery.count);
        g_discovery.results = NULL;
        g_discovery.count = 0;
    }

    AIRY_LOG_INFO("PluginDiscovery: destroyed");
    g_discovery.initialized = false;
}

int plugin_discovery_parse_manifest(const char *yaml_path, const char *plugin_dir,
                                    plugin_discovery_result_t *out_result)
{
    if (!yaml_path || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(out_result, 0, sizeof(*out_result));

    FILE *fp = fopen(yaml_path, "r");
    if (!fp) {
        AIRY_LOG_WARN("PluginDiscovery: cannot open manifest '%s'", yaml_path);
        out_result->valid = false;
        safe_strcpy(out_result->error_reason, "Cannot open manifest file",
                    sizeof(out_result->error_reason));
        return AIRY_ERR_IO;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *value = NULL;

        if ((value = parse_yaml_value(line, "name"))) {
            safe_strcpy(out_result->name, value, sizeof(out_result->name));
        } else if ((value = parse_yaml_value(line, "version"))) {
            safe_strcpy(out_result->version, value, sizeof(out_result->version));
        } else if ((value = parse_yaml_value(line, "author"))) {
            safe_strcpy(out_result->author, value, sizeof(out_result->author));
        } else if ((value = parse_yaml_value(line, "description"))) {
            safe_strcpy(out_result->description, value, sizeof(out_result->description));
        } else if ((value = parse_yaml_value(line, "type"))) {
            out_result->type = parse_plugin_type(value);
        } else if ((value = parse_yaml_value(line, "api_version"))) {
            out_result->api_version = (uint32_t)atoi(value);
        } else if ((value = parse_yaml_value(line, "min_airy_version"))) {
            out_result->min_airy_version = (uint32_t)atoi(value);
        } else if ((value = parse_yaml_value(line, "library"))) {

            if (plugin_dir) {
                snprintf(out_result->library_path, sizeof(out_result->library_path), "%s/%s",
                         plugin_dir, value);
            } else {
                safe_strcpy(out_result->library_path, value, sizeof(out_result->library_path));
            }
        } else if (strncmp(line, "  - ", 4) == 0) {

            char *perm = parse_yaml_value(line, "-");
            if (perm && out_result->permission_count < PLUGIN_DISCOVERY_MAX_PERMISSIONS) {
                safe_strcpy(out_result->permissions[out_result->permission_count], perm, 64);
                out_result->permission_count++;
            }
        }

        if (value)
            AIRY_FREE(value);
    }

    fclose(fp);

    if (out_result->name[0] == '\0') {
        out_result->valid = false;
        safe_strcpy(out_result->error_reason, "Missing required field: name",
                    sizeof(out_result->error_reason));
        AIRY_LOG_WARN("PluginDiscovery: invalid manifest '%s': %s", yaml_path,
                      out_result->error_reason);
        return AIRY_ERR_PARSE_ERROR;
    }

    if (out_result->library_path[0] == '\0') {
        out_result->valid = false;
        safe_strcpy(out_result->error_reason, "Missing required field: library",
                    sizeof(out_result->error_reason));
        return AIRY_ERR_PARSE_ERROR;
    }

    out_result->valid = true;

    AIRY_LOG_DEBUG("PluginDiscovery: parsed manifest '%s' → name=%s "
                   "type=%d version=%s perms=%u",
                   yaml_path, out_result->name, out_result->type, out_result->version,
                   out_result->permission_count);
    return 0;
}

int plugin_discovery_scan(plugin_discovery_result_t **out_results, size_t *out_count)
{
    if (!out_results || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_results = NULL;
    *out_count = 0;

    if (!g_discovery.initialized) {
        AIRY_LOG_WARN("PluginDiscovery: not initialized");
        return AIRY_ERR_SYS_NOT_INIT;
    }

    const char *plugins_dir = g_discovery.plugins_dir;

    if (!dir_exists(plugins_dir)) {
        AIRY_LOG_INFO("PluginDiscovery: plugins dir not found '%s', "
                      "skipping scan",
                      plugins_dir);
        return 0;
    }

    AIRY_LOG_INFO("PluginDiscovery: scanning '%s'...", plugins_dir);

    /* Cross-platform directory traversal setup:
     * POSIX: opendir/readdir/closedir
     * Windows: FindFirstFile/FindNextFile/FindClose */
#ifndef _WIN32
    DIR *dir = opendir(plugins_dir);
    if (!dir) {
        AIRY_LOG_ERROR("PluginDiscovery: cannot open dir '%s'", plugins_dir);
        return AIRY_ERR_IO;
    }
#else
    char win_pattern[PLUGIN_DISCOVERY_MAX_PATH];
    snprintf(win_pattern, sizeof(win_pattern), "%s\\*", plugins_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(win_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        AIRY_LOG_ERROR("PluginDiscovery: cannot open dir '%s'", plugins_dir);
        return AIRY_ERR_IO;
    }
#endif

    plugin_discovery_result_t *results =
        (plugin_discovery_result_t *)AIRY_CALLOC(PLUGIN_DISCOVERY_MAX_PLUGINS,
                                                 sizeof(plugin_discovery_result_t));
    if (!results) {
#ifndef _WIN32
        closedir(dir);
#else
        FindClose(hFind);
#endif
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t found = 0;
#ifdef _WIN32
    int first_entry = 1;
#endif

    while (found < PLUGIN_DISCOVERY_MAX_PLUGINS) {
        const char *d_name;
#ifndef _WIN32
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        d_name = entry->d_name;
#else
        if (!first_entry) {
            if (!FindNextFileA(hFind, &fd))
                break;
        }
        first_entry = 0;
        d_name = fd.cFileName;
#endif

        if (d_name[0] == '.')
            continue;

        char plugin_dir_path[PLUGIN_DISCOVERY_MAX_PATH];
        snprintf(plugin_dir_path, sizeof(plugin_dir_path), "%s/%s", plugins_dir, d_name);

        struct stat st;
        if (stat(plugin_dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        char manifest_path[PLUGIN_DISCOVERY_MAX_PATH];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.yaml", plugin_dir_path);

        if (!file_exists(manifest_path)) {
            AIRY_LOG_DEBUG("PluginDiscovery: skipping '%s' (no manifest.yaml)", d_name);
            continue;
        }

        plugin_discovery_result_t *result = &results[found];
        int ret = plugin_discovery_parse_manifest(manifest_path, plugin_dir_path, result);

        if (ret == 0 && result->valid) {
            found++;
            AIRY_LOG_INFO("PluginDiscovery: found plugin '%s' v%s (type=%d)", result->name,
                          result->version, result->type);
        } else if (g_discovery.config.fail_on_invalid) {
            AIRY_LOG_ERROR("PluginDiscovery: invalid plugin '%s' in '%s'", d_name, plugin_dir_path);
        }
    }

#ifndef _WIN32
    closedir(dir);
#else
    FindClose(hFind);
#endif

    *out_results = results;
    *out_count = found;

    if (g_discovery.results) {
        plugin_discovery_free_results(g_discovery.results, g_discovery.count);
    }
    g_discovery.results = results;
    g_discovery.count = found;

    AIRY_LOG_INFO("PluginDiscovery: scan complete (%zu plugins found)", found);
    return 0;
}

int plugin_discovery_auto_load(void)
{
    if (!g_discovery.initialized) {
        plugin_discovery_init(NULL);
    }

    plugin_discovery_result_t *results = NULL;
    size_t count = 0;

    int ret = plugin_discovery_scan(&results, &count);
    if (ret != 0 || count == 0) {
        AIRY_LOG_INFO("PluginDiscovery: no plugins to auto-load");
        return 0;
    }

    size_t loaded = 0;
    size_t failed = 0;

    for (size_t i = 0; i < count; i++) {
        if (!results[i].valid)
            continue;

        AIRY_LOG_INFO("PluginDiscovery: auto-loading '%s' from '%s'", results[i].name,
                      results[i].library_path);

        const char *out_name = NULL;
        int load_ret = plugin_service_load(results[i].library_path, NULL, &out_name);

        if (load_ret == 0) {
            loaded++;

            plugin_service_start(results[i].name);
        } else {
            failed++;
            AIRY_LOG_WARN("PluginDiscovery: auto-load failed for '%s'", results[i].name);
        }
    }

    AIRY_LOG_INFO("PluginDiscovery: auto-load complete "
                  "(loaded=%zu, failed=%zu)",
                  loaded, failed);

    plugin_discovery_free_results(results, count);
    return (failed > 0) ? -1 : 0;
}

size_t plugin_discovery_count(void)
{
    return g_discovery.count;
}

void plugin_discovery_free_results(plugin_discovery_result_t *results, size_t count)
{
    (void)count;
    if (results) {
        AIRY_FREE(results);
    }
}