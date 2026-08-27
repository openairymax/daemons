// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file unified_metrics.c
 * @brief Unified metrics collector implementation.
 *
 * @see unified_metrics.h
 */

#include "unified_metrics.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

static struct {
    um_config_t config;
    um_module_metrics_t modules[UM_MAX_MODULES];
    uint32_t module_count;
    um_stats_t stats;
    bool initialized;
    airy_mtx_t mutex;
} g_um = {0};

static um_module_metrics_t *find_module(const char *name)
{
    for (uint32_t i = 0; i < g_um.module_count; i++) {
        if (strcmp(g_um.modules[i].module_name, name) == 0)
            return &g_um.modules[i];
    }
    /* "Not found" is normal control flow (the caller checks the return
     * value and logs the specific error), so do not allocate an error
     * context here - that would leak memory. Previously this also wrongly
     * used AIRY_ERR_OVERFLOW ("limit exceeded"), which did not match the
     * semantics. */
    return NULL;
}

static um_metric_entry_t *find_metric(um_module_metrics_t *mod, const char *name)
{
    for (uint32_t i = 0; i < mod->metric_count; i++) {
        if (strcmp(mod->metrics[i].name, name) == 0)
            return &mod->metrics[i];
    }

    return NULL;
}

static int sanitize_prom_name(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0)
        return AIRY_ERR_INVALID_PARAM;
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_size - 1; i++) {
        char ch = in[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '_') {
            out[j++] = ch;
        } else if (ch == '.' || ch == '-' || ch == ' ') {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
    return (j > 0) ? 0 : AIRY_ERR_INVALID_PARAM;
}

static const char *metric_type_string(um_metric_type_t type)
{
    switch (type) {
    case UM_TYPE_COUNTER:
        return "counter";
    case UM_TYPE_GAUGE:
        return "gauge";
    case UM_TYPE_HISTOGRAM:
        return "histogram";
    case UM_TYPE_SUMMARY:
        return "summary";
    default:
        return "untyped";
    }
}

AIRY_API um_config_t um_create_default_config(void)
{
    um_config_t config;
    __builtin_memset(&config, 0, sizeof(um_config_t));
    safe_strcpy(config.service_name, "agentrt", sizeof(config.service_name));
    config.scrape_interval_ms = 15000;
    config.retention_ms = 300000;
    config.enable_default_metrics = true;
    return config;
}

AIRY_API int um_init(const um_config_t *config)
{
    if (g_um.initialized)
        return 0;

    if (config) {
        __builtin_memcpy(&g_um.config, config, sizeof(um_config_t));
    } else {
        g_um.config = um_create_default_config();
    }

    airy_err_t err = airy_mtx_init(&g_um.mutex);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("um_init: mutex init failed err=%d", err);
        return AIRY_ERR_UNKNOWN;
    }

    __builtin_memset(g_um.modules, 0, sizeof(g_um.modules));
    g_um.module_count = 0;
    __builtin_memset(&g_um.stats, 0, sizeof(um_stats_t));
    g_um.initialized = true;

    if (g_um.config.enable_default_metrics) {
        um_register_default_metrics();
    }

    AIRY_LOG_INFO("Unified metrics collector initialized (service=%s)", g_um.config.service_name);
    return 0;
}

AIRY_API void um_shutdown(void)
{
    if (!g_um.initialized)
        return;

    airy_mtx_lock(&g_um.mutex);
    g_um.initialized = false;
    airy_mtx_unlock(&g_um.mutex);

    airy_mtx_destroy(&g_um.mutex);

    AIRY_LOG_INFO("Unified metrics collector shutdown");
}

AIRY_API bool um_is_initialized(void)
{
    return g_um.initialized;
}

AIRY_API int um_register_module(const char *module_name, const char *instance_id)
{
    if (!module_name) {
        SVC_LOG_ERROR("um_register_module: null module_name parameter");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!g_um.initialized)
        um_init(NULL);

    airy_mtx_lock(&g_um.mutex);

    if (find_module(module_name)) {
        airy_mtx_unlock(&g_um.mutex);
        return 0;
    }

    if (g_um.module_count >= UM_MAX_MODULES) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_register_module: max modules reached count=%u max=%u module='%s'",
                      g_um.module_count, UM_MAX_MODULES, module_name);
        AIRY_LOG_ERROR("Max metric modules reached");
        return AIRY_ERR_OVERFLOW;
    }

    um_module_metrics_t *mod = &g_um.modules[g_um.module_count];
    __builtin_memset(mod, 0, sizeof(um_module_metrics_t));
    safe_strcpy(mod->module_name, module_name, UM_MODULE_NAME_LEN);
    if (instance_id) {
        safe_strcpy(mod->instance_id, instance_id, sizeof(mod->instance_id));
    } else {
        snprintf(mod->instance_id, sizeof(mod->instance_id), "%u",
#ifdef _WIN32
                 (uint32_t)GetCurrentProcessId()
#else
                 (uint32_t)getpid()
#endif
        );
    }
    mod->active = true;
    g_um.module_count++;
    g_um.stats.active_modules = g_um.module_count;

    airy_mtx_unlock(&g_um.mutex);

    AIRY_LOG_INFO("Metrics module registered: %s (instance=%s)", module_name, mod->instance_id);
    return 0;
}

AIRY_API int um_unregister_module(const char *module_name)
{
    if (!module_name) {
        SVC_LOG_ERROR("um_unregister_module: null module_name parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_um.mutex);

    um_module_metrics_t *mod = find_module(module_name);
    if (!mod) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_WARN("um_unregister_module: module '%s' not found", module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    uint32_t idx = (uint32_t)(mod - g_um.modules);
    g_um.stats.total_metrics -= mod->metric_count;

    if (idx < g_um.module_count - 1) {
        g_um.modules[idx] = g_um.modules[g_um.module_count - 1];
    }
    __builtin_memset(&g_um.modules[g_um.module_count - 1], 0, sizeof(um_module_metrics_t));
    g_um.module_count--;
    g_um.stats.active_modules = g_um.module_count;

    airy_mtx_unlock(&g_um.mutex);

    return 0;
}

AIRY_API int um_register_metric(const char *module_name, const char *name, um_metric_type_t type,
                                const char *help, const char *labels)
{
    if (!module_name || !name) {
        SVC_LOG_ERROR("um_register_metric: null parameter module_name=%p name=%p",
                      (void *)module_name, (void *)name);
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!g_um.initialized)
        um_init(NULL);

    airy_mtx_lock(&g_um.mutex);

    um_module_metrics_t *mod = find_module(module_name);
    if (!mod) {
        airy_mtx_unlock(&g_um.mutex);
        um_register_module(module_name, NULL);
        airy_mtx_lock(&g_um.mutex);
        mod = find_module(module_name);
        if (!mod) {
            airy_mtx_unlock(&g_um.mutex);
            SVC_LOG_ERROR("um_register_metric: failed to find/create module '%s'", module_name);
            return AIRY_ERR_NOT_FOUND;
        }
    }

    if (find_metric(mod, name)) {
        airy_mtx_unlock(&g_um.mutex);
        return 0;
    }

    if (mod->metric_count >= UM_MAX_METRICS_PER_MOD) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR(
            "um_register_metric: max metrics reached for module '%s' count=%u max=%u metric='%s'",
            module_name, mod->metric_count, UM_MAX_METRICS_PER_MOD, name);
        return AIRY_ERR_OVERFLOW;
    }

    um_metric_entry_t *entry = &mod->metrics[mod->metric_count];
    __builtin_memset(entry, 0, sizeof(um_metric_entry_t));
    safe_strcpy(entry->name, name, UM_METRIC_NAME_LEN);
    if (help)
        safe_strcpy(entry->help, help, sizeof(entry->help));
    if (labels)
        safe_strcpy(entry->labels, labels, sizeof(entry->labels));
    entry->type = type;
    entry->timestamp_ms = airy_time_ms();
    mod->metric_count++;

    g_um.stats.total_registrations++;
    g_um.stats.total_metrics = 0;
    for (uint32_t i = 0; i < g_um.module_count; i++) {
        g_um.stats.total_metrics += g_um.modules[i].metric_count;
    }

    airy_mtx_unlock(&g_um.mutex);

    return 0;
}

AIRY_API int um_increment(const char *module_name, const char *name, uint64_t value)
{
    if (!module_name || !name) {
        SVC_LOG_ERROR("um_increment: null parameter module_name=%p name=%p", (void *)module_name,
                      (void *)name);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_um.mutex);

    um_module_metrics_t *mod = find_module(module_name);
    if (!mod) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_increment: module '%s' not found", module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    um_metric_entry_t *entry = find_metric(mod, name);
    if (!entry) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_increment: metric '%s' not found in module '%s'", name, module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    entry->value += (double)value;
    entry->count += value;
    entry->timestamp_ms = airy_time_ms();

    g_um.stats.total_increments++;

    airy_mtx_unlock(&g_um.mutex);
    return 0;
}

AIRY_API int um_gauge_set(const char *module_name, const char *name, double value)
{
    if (!module_name || !name) {
        SVC_LOG_ERROR("um_gauge_set: null parameter module_name=%p name=%p", (void *)module_name,
                      (void *)name);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_um.mutex);

    um_module_metrics_t *mod = find_module(module_name);
    if (!mod) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_gauge_set: module '%s' not found", module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    um_metric_entry_t *entry = find_metric(mod, name);
    if (!entry) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_gauge_set: metric '%s' not found in module '%s'", name, module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    entry->value = value;
    entry->timestamp_ms = airy_time_ms();

    g_um.stats.total_updates++;

    airy_mtx_unlock(&g_um.mutex);
    return 0;
}

AIRY_API int um_observe(const char *module_name, const char *name, double value)
{
    if (!module_name || !name) {
        SVC_LOG_ERROR("um_observe: null parameter module_name=%p name=%p", (void *)module_name,
                      (void *)name);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_um.mutex);

    um_module_metrics_t *mod = find_module(module_name);
    if (!mod) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_observe: module '%s' not found", module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    um_metric_entry_t *entry = find_metric(mod, name);
    if (!entry) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_observe: metric '%s' not found in module '%s'", name, module_name);
        return AIRY_ERR_NOT_FOUND;
    }

    entry->sum += value;
    entry->count++;
    entry->value = entry->count > 0 ? entry->sum / entry->count : 0;
    entry->timestamp_ms = airy_time_ms();

    g_um.stats.total_updates++;

    airy_mtx_unlock(&g_um.mutex);
    return 0;
}

AIRY_API char *um_export_prometheus(void)
{
    return um_export_prometheus_module(NULL);
}

AIRY_API char *um_export_prometheus_module(const char *module_name)
{
    if (!g_um.initialized) {
        SVC_LOG_ERROR("um_export_prometheus_module: metrics not initialized");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    airy_mtx_lock(&g_um.mutex);

    size_t buf_size = 8192;
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_export_prometheus_module: memory allocation failed for export buffer");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    size_t pos = 0;

#define PAPPEND(fmt, ...)                                                                          \
    do {                                                                                           \
        int w =                                                                                    \
            snprintf(buf + pos, buf_size - pos, fmt,                                               \
                     ##__VA_ARGS__); /* flawfinder: ignore - bounded snprintf in PAPPEND macro */  \
        if (w < 0) {                                                                               \
            AIRY_FREE(buf);                                                                        \
            airy_mtx_unlock(&g_um.mutex);                                                          \
            return NULL;                                                                           \
        }                                                                                          \
        if ((size_t)w >= buf_size - pos) {                                                         \
            buf_size *= 2;                                                                         \
            char *nb = (char *)AIRY_MALLOC(buf_size);                                              \
            if (!nb) {                                                                             \
                AIRY_FREE(buf);                                                                    \
                airy_mtx_unlock(&g_um.mutex);                                                      \
                return NULL;                                                                       \
            }                                                                                      \
            __builtin_memcpy(nb, buf, pos);                                                        \
            AIRY_FREE(buf);                                                                        \
            buf = nb;                                                                              \
            w = snprintf(buf + pos, buf_size - pos, fmt,                                           \
                         ##__VA_ARGS__); /* flawfinder: ignore - bounded realloc+snprintf retry */ \
            if (w < 0 || (size_t)w >= buf_size - pos) {                                            \
                AIRY_FREE(buf);                                                                    \
                airy_mtx_unlock(&g_um.mutex);                                                      \
                return NULL;                                                                       \
            }                                                                                      \
        }                                                                                          \
        pos += (size_t)w;                                                                          \
    } while (0)

    char safe_name[256];
    char prefixed_name[384];

    for (uint32_t m = 0; m < g_um.module_count; m++) {
        um_module_metrics_t *mod = &g_um.modules[m];

        if (module_name && strcmp(mod->module_name, module_name) != 0)
            continue;

        for (uint32_t i = 0; i < mod->metric_count; i++) {
            um_metric_entry_t *entry = &mod->metrics[i];

            if (sanitize_prom_name(entry->name, safe_name, sizeof(safe_name)) != 0)
                continue;

            snprintf(prefixed_name, sizeof(prefixed_name), "airy_%s_%s", mod->module_name,
                     safe_name);

            if (entry->help[0]) {
                PAPPEND("# HELP %s %s\n", prefixed_name, entry->help);
            }
            PAPPEND("# TYPE %s %s\n", prefixed_name, metric_type_string(entry->type));

            if (entry->labels[0]) {
                PAPPEND("%s{%s,module=\"%s\",instance=\"%s\"}", prefixed_name, entry->labels,
                        mod->module_name, mod->instance_id);
            } else {
                PAPPEND("%s{module=\"%s\",instance=\"%s\"}", prefixed_name, mod->module_name,
                        mod->instance_id);
            }

            switch (entry->type) {
            case UM_TYPE_COUNTER:
                PAPPEND(" %.17g\n", entry->value);
                break;
            case UM_TYPE_GAUGE:
                PAPPEND(" %.17g\n", entry->value);
                break;
            case UM_TYPE_HISTOGRAM:
            case UM_TYPE_SUMMARY:
                PAPPEND("_sum %.17g\n%s_count{module=\"%s\",instance=\"%s\"} %llu\n", entry->sum,
                        prefixed_name, mod->module_name, mod->instance_id,
                        (unsigned long long)entry->count);
                break;
            }
        }
    }

#undef PAPPEND

    g_um.stats.total_exports++;
    airy_mtx_unlock(&g_um.mutex);

    return buf;
}

AIRY_API char *um_export_json(void)
{
    if (!g_um.initialized) {
        SVC_LOG_ERROR("um_export_json: metrics not initialized");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    airy_mtx_lock(&g_um.mutex);

    size_t buf_size = 8192;
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf) {
        airy_mtx_unlock(&g_um.mutex);
        SVC_LOG_ERROR("um_export_json: memory allocation failed for export buffer");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    size_t pos = 0;

    pos += snprintf(buf + pos, buf_size - pos, "{\"service\":\"%s\",\"modules\":{",
                    g_um.config.service_name);

    for (uint32_t m = 0; m < g_um.module_count; m++) {
        um_module_metrics_t *mod = &g_um.modules[m];
        if (m > 0)
            pos += snprintf(buf + pos, buf_size - pos, ",");
        pos += snprintf(buf + pos, buf_size - pos, "\"%s\":{", mod->module_name);

        for (uint32_t i = 0; i < mod->metric_count; i++) {
            um_metric_entry_t *entry = &mod->metrics[i];
            if (i > 0)
                pos += snprintf(buf + pos, buf_size - pos, ",");
            pos += snprintf(buf + pos, buf_size - pos,
                            "\"%s\":{\"type\":\"%s\",\"value\":%.17g,\"count\":%llu}", entry->name,
                            metric_type_string(entry->type), entry->value,
                            (unsigned long long)entry->count);
        }
        pos += snprintf(buf + pos, buf_size - pos, "}");
    }

    pos += snprintf(buf + pos, buf_size - pos, "}}");

    g_um.stats.total_exports++;
    airy_mtx_unlock(&g_um.mutex);

    return buf;
}

AIRY_API int um_register_default_metrics(void)
{
    um_register_module("system", "default");

    um_register_metric("system", "process_cpu_seconds", UM_TYPE_COUNTER,
                       "Total CPU seconds used by the process", NULL);
    um_register_metric("system", "process_memory_bytes", UM_TYPE_GAUGE,
                       "Process memory usage in bytes", NULL);
    um_register_metric("system", "process_open_fds", UM_TYPE_GAUGE,
                       "Number of open file descriptors", NULL);
    um_register_metric("system", "process_threads", UM_TYPE_GAUGE, "Number of threads", NULL);
    um_register_metric("system", "process_uptime_seconds", UM_TYPE_COUNTER,
                       "Process uptime in seconds", NULL);

    um_register_module("airy_runtime", "default");

    um_register_metric("airy_runtime", "requests_total", UM_TYPE_COUNTER,
                       "Total number of requests processed", "method=\"\",path=\"\"");
    um_register_metric("airy_runtime", "request_duration_seconds", UM_TYPE_SUMMARY,
                       "Request duration in seconds", "method=\"\",path=\"\"");
    um_register_metric("airy_runtime", "errors_total", UM_TYPE_COUNTER, "Total number of errors",
                       "type=\"\"");
    um_register_metric("airy_runtime", "active_sessions", UM_TYPE_GAUGE,
                       "Number of active sessions", NULL);
    um_register_metric("airy_runtime", "active_connections", UM_TYPE_GAUGE,
                       "Number of active connections", NULL);

    return 0;
}

AIRY_API void um_update_default_metrics(void)
{
    if (!g_um.initialized)
        return;

#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        um_gauge_set("system", "process_memory_bytes", (double)ms.dwMemoryLoad);
    }
#elif defined(__APPLE__)
    /* macOS 无 /proc：用 task_info(TASK_VM_INFO) 取进程常驻内存（resident_size）。
     * mach/mach.h 声明 task_info；mach_task_self() 为自进程任务端口。 */
    {
        mach_msg_type_number_t info_count = TASK_VM_INFO_COUNT;
        task_vm_info_data_t vm_info;
        kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                     (task_info_t)&vm_info, &info_count);
        if (kr == KERN_SUCCESS) {
            um_gauge_set("system", "process_memory_bytes", (double)vm_info.resident_size);
        }
    }
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long rss = 0;
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            char *saveptr = NULL;
            char *tok = strtok_r(line, " \t", &saveptr);
            tok = strtok_r(NULL, " \t", &saveptr); /* skip first field */
            if (tok) {
                rss = strtol(tok, NULL, 10);
                um_gauge_set("system", "process_memory_bytes", (double)(rss * 4096));
            }
        }
        fclose(f);
    }
#endif

    uint64_t uptime __attribute__((unused)) = airy_time_ms() / 1000;
    um_increment("system", "process_uptime_seconds", 1);
}

AIRY_API int um_get_stats(um_stats_t *stats)
{
    if (!stats) {
        SVC_LOG_ERROR("um_get_stats: null stats parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_um.mutex);
    __builtin_memcpy(stats, &g_um.stats, sizeof(um_stats_t));
    airy_mtx_unlock(&g_um.mutex);

    return 0;
}
