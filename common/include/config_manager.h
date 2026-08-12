/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file config_manager.h
 * @brief Unified configuration management system.
 *
 * Cross-daemon unified configuration management supporting:
 * - Multi-format config (JSON/YAML/INI/ENV)
 * - Hot config reload (file watch + callback notification)
 * - Config versioning (change history + rollback)
 * - Environment-specific config (dev/staging/prod)
 * - Config validation and defaults
 * - Cross-process config sync (shared memory based)
 *
 * @see svc_common.h service-management framework
 */

#ifndef AIRY_RT_CONFIG_MANAGER_H
#define AIRY_RT_CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define CM_MAX_KEY_LEN 128
#define CM_MAX_VALUE_LEN 2048
#define CM_MAX_ENTRIES 512
#define CM_MAX_WATCHERS 32
#define CM_MAX_HISTORY 64
#define CM_MAX_NAMESPACE_LEN 32
#define CM_MAX_PATH_LEN 512


typedef enum {
    CM_TYPE_STRING = 0,
    CM_TYPE_INT = 1,
    CM_TYPE_DOUBLE = 2,
    CM_TYPE_BOOL = 3,
    CM_TYPE_NULL = 4
} cm_value_type_t;


typedef struct {
    char key[CM_MAX_KEY_LEN];
    char value[CM_MAX_VALUE_LEN];
    cm_value_type_t type;
    char namespace_[CM_MAX_NAMESPACE_LEN];
    uint64_t version;
    uint64_t last_modified;
    bool is_default;
    bool is_overridden;
    char source[64];
} cm_entry_t;


typedef struct {
    char key[CM_MAX_KEY_LEN];
    char old_value[CM_MAX_VALUE_LEN];
    char new_value[CM_MAX_VALUE_LEN];
    uint64_t timestamp;
    char source[64];
} cm_change_record_t;


typedef struct {
    char base_path[CM_MAX_PATH_LEN];
    char environment[32];
    uint32_t watch_interval_ms;
    uint32_t max_history;
    bool enable_hot_reload;
    bool enable_validation;
    bool enable_cross_process_sync;
    char sync_shm_name[256];
} cm_config_t;


typedef void (*cm_change_callback_t)(const char *key, const char *old_value, const char *new_value,
                                     void *user_data);


typedef bool (*cm_validator_t)(const char *key, const char *value, char *error_msg,
                               size_t error_msg_size);


/**
 * @brief Initialize the config manager.
 * @param config Config params (NULL = defaults)
 * @return 0 on success, non-zero on failure
 */
int cm_init(const cm_config_t *config);

/** @brief Shut down the config manager. */
void cm_shutdown(void);


/**
 * @brief Get a config value.
 * @param key Config key (format: namespace.key or key)
 * @param default_value Default value (returned when key absent)
 * @return Config value string
 */
const char *cm_get(const char *key, const char *default_value);

/**
 * @brief Get an integer config value.
 * @param key Config key
 * @param default_value Default value
 * @return Config value
 */
int64_t cm_get_int(const char *key, int64_t default_value);

/**
 * @brief Get a floating-point config value.
 * @param key Config key
 * @param default_value Default value
 * @return Config value
 */
double cm_get_double(const char *key, double default_value);

/**
 * @brief Get a boolean config value.
 * @param key Config key
 * @param default_value Default value
 * @return Config value
 */
bool cm_get_bool(const char *key, bool default_value);

/**
 * @brief Set a config value.
 * @param key Config key
 * @param value Config value
 * @param source Source identifier
 * @return 0 on success, non-zero on failure
 */
int cm_set(const char *key, const char *value, const char *source);

/**
 * @brief Set a namespaced config value.
 * @param namespace_ Namespace
 * @param key Config key
 * @param value Config value
 * @param source Source identifier
 * @return 0 on success, non-zero on failure
 */
int cm_set_namespaced(const char *namespace_, const char *key, const char *value,
                      const char *source);


/**
 * @brief Load config from a config file.
 *
 * Supports JSON, YAML and INI config files. The format is auto-detected
 * and the matching parser used. Simple key=value or key: value lines are
 * parsed line-wise.
 *
 * @param path File path
 * @param namespace_ Namespace (NULL = default)
 * @return Number of loaded items, -1 on failure
 */
int cm_load_json(const char *path, const char *namespace_);

/**
 * @brief Load config from environment variables.
 * @param prefix Env-var prefix (e.g. "AIRY_")
 * @param namespace_ Namespace
 * @return Number of loaded items
 */
int cm_load_env(const char *prefix, const char *namespace_);

/**
 * @brief Load config from command-line arguments.
 * @param argc Argument count
 * @param argv Argument array
 * @return Number of loaded items
 */
int cm_load_args(int argc, char **argv);


/**
 * @brief Register a config-change callback.
 * @param key_pattern Key pattern (supports * wildcards, NULL = watch all)
 * @param callback Callback function
 * @param user_data User data
 * @return 0 on success, non-zero on failure
 */
int cm_watch(const char *key_pattern, cm_change_callback_t callback, void *user_data);

/**
 * @brief Cancel a config watch.
 * @param key_pattern Key pattern
 * @param callback Callback function
 * @return 0 on success, non-zero on failure
 */
int cm_unwatch(const char *key_pattern, cm_change_callback_t callback);

/**
 * @brief Manually trigger a config reload.
 * @return 0 on success, non-zero on failure
 */
int cm_reload(void);


/**
 * @brief Register a config validator.
 * @param key_pattern Key pattern
 * @param validator Validator function
 * @return 0 on success, non-zero on failure
 */
int cm_register_validator(const char *key_pattern, cm_validator_t validator);

/**
 * @brief Validate all config.
 * @return Number of failed validations
 */
int cm_validate_all(void);


/**
 * @brief Get the config change history.
 * @param key Config key (NULL = all)
 * @param records [out] Change-record array
 * @param max_count Array capacity
 * @param found_count [out] Actual count
 * @return 0 on success, non-zero on failure
 */
int cm_get_history(const char *key, cm_change_record_t *records, uint32_t max_count,
                   uint32_t *found_count);

/**
 * @brief Roll config back to a given version.
 * @param key Config key
 * @param version Target version (0 = previous version)
 * @return 0 on success, non-zero on failure
 */
int cm_rollback(const char *key, uint64_t version);


/**
 * @brief Get the current environment.
 * @return Environment name
 */
const char *cm_get_environment(void);

/**
 * @brief Set the environment.
 * @param env Environment name (dev/staging/prod)
 * @return 0 on success, non-zero on failure
 */
int cm_set_environment(const char *env);

/**
 * @brief Load environment-specific config.
 * @param env Environment name
 * @return Number of loaded items
 */
int cm_load_environment_config(const char *env);


/**
 * @brief Export config as a JSON string.
 * @param namespace_ Namespace (NULL = export all)
 * @return JSON string (caller frees), NULL on failure
 */
char *cm_export_json(const char *namespace_);

/**
 * @brief Get the number of config entries.
 * @return Entry count
 */
uint32_t cm_entry_count(void);

/**
 * @brief Create a default config.
 * @return Default config
 */
cm_config_t cm_create_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_CONFIG_MANAGER_H */
