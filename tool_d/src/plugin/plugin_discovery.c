// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plugin_discovery.c
 * @brief P2.2.1: plugin-discovery implementation - scan directories +
 *        parse manifest.yaml.
 *
 * Scans every subdirectory under ecosystem/plugins/, finds and parses
 * manifest.yaml, extracting plugin metadata.
 */

#include "plugin_discovery.h"
#include "plugin_service.h"
#include "safe_string_utils.h"

#include "logger.h"
#include "airy_memory.h"
#include "string_compat.h"
#include "platform.h"

#include <stdarg.h>
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

static struct {
    plugin_discovery_config_t config;
    char plugins_dir[PLUGIN_DISCOVERY_MAX_PATH];
    plugin_discovery_result_t *results;
    size_t count;
    bool initialized;
} g_discovery;

/* Manifest 校验失败的单一出口：写原因、标记 invalid 并返回错误码。
 * out 可为 NULL（validate_plugin 的探测式调用）。 */
static int disco_reject(plugin_discovery_result_t *out, int err,
                        const char *fmt, ...)
{
    if (out) {
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(out->error_reason, sizeof(out->error_reason), fmt, ap);
        va_end(ap);
        out->valid = false;
    }
    return err;
}

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
 * @brief Parse the plugin type string.
 *
 * 0.1.6 生态 SSoT（S-4）：未知 type 返回 PLUGIN_TYPE_COUNT 哨兵，
 * 由调用方判为非法 manifest（此前静默回退 TOOL_PROVIDER，掩盖
 * 拼写错误）。
 */
static plugin_type_t parse_plugin_type(const char *type_str)
{
    if (!type_str)
        return PLUGIN_TYPE_COUNT;

    if (strcmp(type_str, "tool_provider") == 0)
        return PLUGIN_TYPE_TOOL_PROVIDER;
    if (strcmp(type_str, "protocol_adapter") == 0)
        return PLUGIN_TYPE_PROTOCOL_ADAPTER;
    if (strcmp(type_str, "memory_provider") == 0)
        return PLUGIN_TYPE_MEMORY_PROVIDER;
    if (strcmp(type_str, "hook_extension") == 0)
        return PLUGIN_TYPE_HOOK_EXTENSION;

    return PLUGIN_TYPE_COUNT;
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
        g_discovery.config.fail_on_invalid = false;
    }

    if (g_discovery.plugins_dir[0] == '\0') {
        /* P1-5：默认目录必须解析为绝对路径 $AIRY_HOME/ecosystem/plugins
         * （与 plugin_d main.c 显式配置同源）。历史上回退值是相对路径
         * "ecosystem/plugins/"，随进程 CWD 漂移，daemon 从其他启动目录
         * 扫描恒为空（根因修复，与调用方两处路径约定一致）。 */
        snprintf(g_discovery.plugins_dir, sizeof(g_discovery.plugins_dir),
                 "%s/ecosystem/plugins", airy_home_dir());
    }

    g_discovery.initialized = true;

    AIRY_LOG_INFO("PluginDiscovery: initialized (dir=%s)",
                  g_discovery.plugins_dir);
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

static int parse_manifest(const char *yaml_path, const char *plugin_dir,
                                           plugin_discovery_result_t *out_result)
{
    if (!yaml_path || !out_result)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(out_result, 0, sizeof(*out_result));

    FILE *fp = fopen(yaml_path, "r");
    if (!fp) {
        AIRY_LOG_WARN("PluginDiscovery: cannot open manifest '%s'", yaml_path);
        return disco_reject(out_result, AIRY_ERR_IO,
                            "Cannot open manifest file");
    }

    char line[512];
    char raw_type[64] = "";
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
            safe_strcpy(raw_type, value, sizeof(raw_type));
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
            AIRY_FREE(perm);
        }

        if (value)
            AIRY_FREE(value);
    }

    fclose(fp);

    if (out_result->name[0] == '\0') {
        int rc = disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                              "Missing required field: name");
        AIRY_LOG_WARN("PluginDiscovery: invalid manifest '%s': %s", yaml_path,
                      out_result->error_reason);
        return rc;
    }

    if (out_result->library_path[0] == '\0')
        return disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                            "Missing required field: library");

    /* 0.1.6 生态 SSoT（S-4）：manifest 必填约束对齐
     * manager/schema/plugin-manifest.schema.json——type 必须存在且合法，
     * api_version / min_airy_version 必填（此前静默以 0/默认 type 接受）。 */
    if (raw_type[0] == '\0')
        return disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                            "Missing required field: type");
    if (out_result->type == PLUGIN_TYPE_COUNT)
        return disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                            "Invalid plugin type: %s (expect "
                            "tool_provider|protocol_adapter|"
                            "memory_provider|hook_extension)",
                            raw_type);
    if (out_result->api_version == 0)
        return disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                            "Missing required field: api_version");
    if (out_result->min_airy_version == 0)
        return disco_reject(out_result, AIRY_ERR_PARSE_ERROR,
                            "Missing required field: min_airy_version");

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

        if (!dir_exists(plugin_dir_path))
            continue;

        char manifest_path[PLUGIN_DISCOVERY_MAX_PATH];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.yaml", plugin_dir_path);

        if (!file_exists(manifest_path)) {
            AIRY_LOG_DEBUG("PluginDiscovery: skipping '%s' (no manifest.yaml)", d_name);
            continue;
        }

        plugin_discovery_result_t *result = &results[found];
        int ret = parse_manifest(manifest_path, plugin_dir_path, result);

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

void plugin_discovery_free_results(plugin_discovery_result_t *results, size_t count)
{
    (void)count;
    if (results) {
        AIRY_FREE(results);
    }
}
