// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service.c
 * @brief P2.2: Plugin service implementation - dynamic load/unload and
 *        lifecycle management.
 *
 * Uses dlopen/dlsym to load dynamic-library plugins and manage their
 * lifecycle. Thread-safe registry supporting the 4 plugin types.
 *
 * Plugin entry-point convention:
 *   - plugin_metadata_fn()  -> returns plugin_metadata_t
 *   - plugin_init_fn()      -> initialize
 *   - plugin_destroy_fn()   -> destroy
 *   - plugin_start_fn()     -> start
 *   - plugin_stop_fn()      -> stop
 *
 */

#include "plugin_service.h"
/* P0.17 phase 2: service.c uses airy_dl_* daemon-specific functions, so
 * include daemon_platform_ext.h for their declarations (the commons
 * platform.h lacks them). */
#include "daemon_platform_ext.h"
#include "error.h"
#include "svc_logger.h"
#include "sync.h"
#include "airy_memory.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* dlsym returns void* (object pointer) but plugin symbols are function
 * pointers. ISO C forbids direct function-pointer <-> object-pointer
 * conversion; the memcpy pattern avoids -Wpedantic warnings. This is the
 * POSIX-recommended way to obtain function pointers via dlsym (avoiding
 * undefined behavior). */
#define PLUGIN_DLSYM_FUNC(handle, name, fn_var)                        \
    do {                                                               \
        void *_plugin_sym = load_symbol((handle), (name));             \
        if (_plugin_sym) {                                             \
            AIRY_MEMCPY(&(fn_var), &_plugin_sym, sizeof(_plugin_sym)); \
        } else {                                                       \
            (fn_var) = NULL;                                           \
        }                                                              \
    } while (0)

#define PLUGIN_MAX_COUNT 64
#define PLUGIN_NAME_MAX_LEN 64

/**
 * @brief Plugin registry node
 */
typedef struct plugin_node {
    plugin_descriptor_t desc;
    plugin_stats_t stats;
    struct timespec load_time;
    struct plugin_node *next;
} plugin_node_t;

/**
 * @brief Global plugin registry
 */
static struct {
    plugin_node_t *head;
    size_t count;
    sync_rwlock_t rwlock;
    bool initialized;
} g_plugin_registry;

/**
 * @brief Find a plugin node by name
 * @return Node pointer, NULL if not found
 */
static plugin_node_t *find_node(const char *name)
{
    if (!name)
        return NULL;
    plugin_node_t *node = g_plugin_registry.head;
    while (node) {
        if (strcmp(node->desc.metadata.name, name) == 0)
            return node;
        node = node->next;
    }
    return NULL;
}

/**
 * @brief Load a symbol from the dynamic library
 */
static void *load_symbol(void *handle, const char *name)
{
    void *sym = airy_dl_sym(handle, name);
    if (!sym) {
        SVC_LOG_ERROR("P2.2: PluginD: Symbol not found: %s (%s)", name, airy_dl_error());
    }
    return sym;
}

/**
 * @brief Initialize the plugin registry
 */
static int registry_init(void)
{
    if (g_plugin_registry.initialized)
        return 0;

    AIRY_MEMSET(&g_plugin_registry, 0, sizeof(g_plugin_registry));
    if (sync_rwlock_create(&g_plugin_registry.rwlock, NULL) != 0)
        return AIRY_ERR_SYS_MUTEX;

    g_plugin_registry.initialized = true;
    return 0;
}

int plugin_service_load(const char *library_path, const char *config_path, const char **out_name)
{
    if (!library_path)
        return AIRY_ERR_INVALID_PARAM;

    if (registry_init() != 0)
        return AIRY_ERR_GENERIC_FAIL;

    void *handle = airy_dl_open(library_path);
    if (!handle) {
        SVC_LOG_ERROR("P2.2: PluginD: dlopen failed: %s (%s)", library_path, airy_dl_error());
        return AIRY_ERR_GENERIC_FAIL;
    }

    typedef const plugin_metadata_t *(*metadata_fn_t)(void);
    metadata_fn_t get_metadata = NULL;
    PLUGIN_DLSYM_FUNC(handle, "plugin_get_metadata", get_metadata);
    if (!get_metadata) {
        airy_dl_close(handle);
        return AIRY_ERR_NOT_FOUND;
    }

    const plugin_metadata_t *metadata = get_metadata();
    if (!metadata || !metadata->name[0]) {
        SVC_LOG_ERROR("P2.2: PluginD: Invalid plugin metadata");
        airy_dl_close(handle);
        return AIRY_ERR_INVALID_PARAM;
    }

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);
    if (find_node(metadata->name)) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        SVC_LOG_WARN("P2.2: PluginD: Plugin already loaded: %s", metadata->name);
        airy_dl_close(handle);
        return AIRY_ERR_ALREADY_EXISTS;
    }
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    plugin_init_fn init_fn = NULL;
    plugin_destroy_fn destroy_fn = NULL;
    plugin_start_fn start_fn = NULL;
    plugin_stop_fn stop_fn = NULL;
    PLUGIN_DLSYM_FUNC(handle, "plugin_init", init_fn);
    PLUGIN_DLSYM_FUNC(handle, "plugin_destroy", destroy_fn);
    PLUGIN_DLSYM_FUNC(handle, "plugin_start", start_fn);
    PLUGIN_DLSYM_FUNC(handle, "plugin_stop", stop_fn);

    if (!init_fn || !destroy_fn) {
        SVC_LOG_ERROR("P2.2: PluginD: Missing required symbols (init/destroy) for %s",
                      metadata->name);
        airy_dl_close(handle);
        return AIRY_ERR_NOT_FOUND;
    }

    plugin_node_t *node = (plugin_node_t *)AIRY_CALLOC(1, sizeof(plugin_node_t));
    if (!node) {
        airy_dl_close(handle);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    AIRY_MEMCPY(&node->desc.metadata, metadata, sizeof(plugin_metadata_t));
    node->desc.init = init_fn;
    node->desc.destroy = destroy_fn;
    node->desc.start = start_fn;
    node->desc.stop = stop_fn;
    node->desc.handle = handle;
    node->desc.user_data = NULL;
    node->desc.state = PLUGIN_STATE_LOADED;

    AIRY_STRNCPY_TERM(node->desc.library_path, library_path, sizeof(node->desc.library_path));
    if (config_path) {
        AIRY_STRNCPY_TERM(node->desc.config_path, config_path, sizeof(node->desc.config_path));
    }

    clock_gettime(CLOCK_MONOTONIC, &node->load_time);
    void *user_data = NULL;
    int init_ret = init_fn(config_path, &user_data);
    if (init_ret != 0) {
        SVC_LOG_ERROR("P2.2: PluginD: Plugin init failed: %s (err=%d)", metadata->name, init_ret);
        node->desc.state = PLUGIN_STATE_ERROR;
        node->stats.error_count++;

    } else {
        node->desc.user_data = user_data;
        node->desc.state = PLUGIN_STATE_INITIALIZED;
    }

    node->stats.load_count++;

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);
    node->next = g_plugin_registry.head;
    g_plugin_registry.head = node;
    g_plugin_registry.count++;
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    if (out_name) {
        *out_name = metadata->name;
    }

    SVC_LOG_INFO("P2.2: PluginD: Plugin loaded: %s v%s (type=%d, state=%d)", metadata->name,
                 metadata->version, metadata->type, node->desc.state);

    return 0;
}

int plugin_service_unload(const char *name)
{
    if (!name)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);

    plugin_node_t **prev = &g_plugin_registry.head;
    while (*prev) {
        plugin_node_t *node = *prev;
        if (strcmp(node->desc.metadata.name, name) == 0) {

            if (node->desc.state == PLUGIN_STATE_RUNNING && node->desc.stop) {
                node->desc.stop(node->desc.user_data);
            }

            if (node->desc.destroy) {
                node->desc.destroy(node->desc.user_data);
            }

            if (node->desc.handle) {
                airy_dl_close(node->desc.handle);
            }

            *prev = node->next;
            AIRY_FREE(node);
            g_plugin_registry.count--;

            sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
            SVC_LOG_INFO("P2.2: PluginD: Plugin unloaded: %s", name);
            return 0;
        }
        prev = &(*prev)->next;
    }

    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
    return AIRY_ERR_NOT_FOUND;
}

int plugin_service_start(const char *name)
{
    if (!name)
        return AIRY_ERR_INVALID_PARAM;

    /* Look up under the lock + snapshot the callback (user callbacks may be
     * slow/blocking during plugin init; moved out of the write lock to avoid
     * blocking all registry reads) */
    int (*start_fn)(void *) = NULL;
    void *user_data = NULL;

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    if (!node) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }

    if (node->desc.state == PLUGIN_STATE_RUNNING) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return 0;
    }

    if (node->desc.state == PLUGIN_STATE_ERROR) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_STATE_ERROR;
    }

    if (node->desc.start) {
        start_fn = node->desc.start;
        user_data = node->desc.user_data;
    }
    node->desc.state = PLUGIN_STATE_STARTING;
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    int ret = 0;
    if (start_fn) {
        ret = start_fn(user_data);
    }

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *n2 = find_node(name);
    if (!n2) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }
    if (ret != 0) {
        n2->desc.state = PLUGIN_STATE_ERROR;
        n2->stats.error_count++;
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        SVC_LOG_ERROR("P2.2: PluginD: Plugin start failed: %s (err=%d)", name, ret);
        return AIRY_ERR_EXEC_FAIL;
    }

    n2->desc.state = PLUGIN_STATE_RUNNING;
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    SVC_LOG_INFO("P2.2: PluginD: Plugin started: %s", name);
    return 0;
}

int plugin_service_stop(const char *name)
{
    if (!name)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_write_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    if (!node) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }

    if (node->desc.state != PLUGIN_STATE_RUNNING) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return 0;
    }

    if (node->desc.stop) {
        node->desc.stop(node->desc.user_data);
    }

    node->desc.state = PLUGIN_STATE_INITIALIZED;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    node->stats.uptime_ns += (uint64_t)(now.tv_sec - node->load_time.tv_sec) * 1000000000ULL +
                             (uint64_t)(now.tv_nsec - node->load_time.tv_nsec);

    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    SVC_LOG_INFO("P2.2: PluginD: Plugin stopped: %s", name);
    return 0;
}

int plugin_service_get_metadata(const char *name, plugin_metadata_t *metadata)
{
    if (!name || !metadata)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    if (!node) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }

    AIRY_MEMCPY(metadata, &node->desc.metadata, sizeof(plugin_metadata_t));
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
    return 0;
}

plugin_state_t plugin_service_get_state(const char *name)
{
    if (!name)
        return PLUGIN_STATE_UNLOADED;

    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    plugin_state_t state = node ? node->desc.state : PLUGIN_STATE_UNLOADED;
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
    return state;
}

int plugin_service_get_stats(const char *name, plugin_stats_t *stats)
{
    if (!name || !stats)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    if (!node) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }

    AIRY_MEMCPY(stats, &node->stats, sizeof(plugin_stats_t));

    if (node->desc.state == PLUGIN_STATE_RUNNING) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        stats->uptime_ns += (uint64_t)(now.tv_sec - node->load_time.tv_sec) * 1000000000ULL +
                            (uint64_t)(now.tv_nsec - node->load_time.tv_nsec);
    }

    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
    return 0;
}

int plugin_service_get_daemon_stats(size_t *out_count, uint64_t *out_loads,
                                    uint64_t *out_errors, uint64_t *out_memory)
{
    if (!out_count || !out_loads || !out_errors || !out_memory)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);
    size_t count = 0;
    uint64_t loads = 0, errors = 0, memory = 0;
    plugin_node_t *node = g_plugin_registry.head;
    while (node) {
        count++;
        loads += node->stats.load_count;
        errors += node->stats.error_count;
        memory += node->stats.memory_bytes;
        node = node->next;
    }
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    *out_count = count;
    *out_loads = loads;
    *out_errors = errors;
    *out_memory = memory;
    return 0;
}

int plugin_service_list(char ***names, size_t *count, int type_filter)
{
    if (!names || !count)
        return AIRY_ERR_INVALID_PARAM;

    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);

    size_t total = 0;
    plugin_node_t *node = g_plugin_registry.head;
    while (node) {
        if (type_filter < 0 || (int)node->desc.metadata.type == type_filter) {
            total++;
        }
        node = node->next;
    }

    char **name_array = (char **)AIRY_CALLOC(total, sizeof(char *));
    if (!name_array && total > 0) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t idx = 0;
    node = g_plugin_registry.head;
    while (node && idx < total) {
        if (type_filter < 0 || (int)node->desc.metadata.type == type_filter) {
            name_array[idx] = AIRY_STRDUP(node->desc.metadata.name);
            idx++;
        }
        node = node->next;
    }

    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    *names = name_array;
    *count = total;
    return 0;
}

int plugin_service_execute(const char *name, const char *json_input, char **json_output)
{
    if (!name || !json_input || !json_output)
        return AIRY_ERR_INVALID_PARAM;
    *json_output = NULL;

    /* Snapshot the exec function and user_data under the lock (same pattern
     * as plugin_service_start, avoiding the dlclose race; user callbacks run
     * outside the lock) */
    int (*exec_fn)(const char *, char **) = NULL;
    sync_rwlock_read_lock_ex(g_plugin_registry.rwlock, NULL);
    plugin_node_t *node = find_node(name);
    if (!node) {
        sync_rwlock_unlock_ex(g_plugin_registry.rwlock);
        return AIRY_ERR_NOT_FOUND;
    }
    void *sym = airy_dl_sym(node->desc.handle, "plugin_execute");
    if (sym) {
        AIRY_MEMCPY(&exec_fn, &sym, sizeof(sym));
    }
    sync_rwlock_unlock_ex(g_plugin_registry.rwlock);

    if (!exec_fn) {
        SVC_LOG_WARN("P2.2: PluginD: plugin '%s' does not export plugin_execute", name);
        return AIRY_ERR_NOT_FOUND;
    }

    int ret = exec_fn(json_input, json_output);
    if (ret != 0) {
        SVC_LOG_ERROR("P2.2: PluginD: plugin_execute failed: %s (err=%d)", name, ret);
        return AIRY_ERR_EXEC_FAIL;
    }
    if (!*json_output) {
        SVC_LOG_ERROR("P2.2: PluginD: plugin_execute returned empty output: %s", name);
        return AIRY_ERR_EXEC_FAIL;
    }
    return 0;
}