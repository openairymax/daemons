// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_apikey.c
 * @brief Daemon auth-middleware API Key domain: key verification and dynamic
 *        add/remove management.
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_auth.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#include "svc_auth_internal.h"

apikey_global_state_t g_apikey = {.initialized = 0};

int auth_apikey_init(const apikey_config_t *config)
{
    if (g_apikey.initialized)
        return AIRY_ERR_ALREADY_INIT;

    airy_mtx_init(&g_apikey.lock);

    if (config) {
        __builtin_memcpy(&g_apikey.config, config, sizeof(apikey_config_t));

        if (config->allowed_keys && config->key_count > 0) {
            g_apikey.capacity = config->key_count + 10;
            g_apikey.keys = (char **)AIRY_CALLOC(g_apikey.capacity, sizeof(*g_apikey.keys));
            if (g_apikey.keys) {
                for (size_t i = 0; i < config->key_count; i++) {
                    if (config->allowed_keys[i]) {
                        g_apikey.keys[i] = AIRY_STRDUP(config->allowed_keys[i]);
                        g_apikey.config.key_count++;
                    }
                }
            }
        }
    } else {

        __builtin_memset(&g_apikey.config, 0, sizeof(apikey_config_t));
        g_apikey.capacity = 10;
        g_apikey.keys = (char **)AIRY_CALLOC(g_apikey.capacity, sizeof(*g_apikey.keys));
        if (!g_apikey.keys) {
            g_apikey.capacity = 0;
            return AUTH_FAILED;
        }
    }

    g_apikey.initialized = 1;
    SVC_LOG_INFO("API Key verification module initialized (%zu keys)", g_apikey.config.key_count);
    return AUTH_SUCCESS;
}

int auth_apikey_verify(const char *api_key, auth_result_t *result)
{
    if (!g_apikey.initialized || !api_key || !result) {
        return AUTH_APIKEY_INVALID;
    }

    __builtin_memset(result, 0, sizeof(auth_result_t));
    result->status = AUTH_FAILED;
    result->error_message = "API Key invalid";

    airy_mtx_lock(&g_apikey.lock);

    for (size_t i = 0; i < g_apikey.config.key_count; i++) {
        if (g_apikey.keys[i] && strcmp(api_key, g_apikey.keys[i]) == 0) {

            result->status = AUTH_SUCCESS;
            result->error_message = NULL;
            AIRY_STRNCPY_TERM(g_apikey.subject_buf, api_key, sizeof(g_apikey.subject_buf));
            (g_apikey.subject_buf)[sizeof(g_apikey.subject_buf) - 1] = '\0';
            g_apikey.subject_buf[sizeof(g_apikey.subject_buf) - 1] = '\0';
            result->subject = g_apikey.subject_buf;
            result->role = "api_user";

            airy_mtx_unlock(&g_apikey.lock);
            SVC_LOG_DEBUG("API Key verified successfully");
            return AUTH_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_apikey.lock);
    SVC_LOG_WARN("API Key verification failed");
    return AUTH_APIKEY_INVALID;
}

int auth_apikey_add(const char *new_key)
{
    if (!g_apikey.initialized || !new_key) {
        return AUTH_APIKEY_INVALID;
    }

    airy_mtx_lock(&g_apikey.lock);

    for (size_t i = 0; i < g_apikey.config.key_count; i++) {
        if (g_apikey.keys[i] && strcmp(new_key, g_apikey.keys[i]) == 0) {
            airy_mtx_unlock(&g_apikey.lock);
            return AIRY_ERR_ALREADY_EXISTS;
        }
    }

    if (g_apikey.config.key_count >= g_apikey.capacity) {
        size_t new_cap = g_apikey.capacity * 2;
        char **new_keys = (char **)AIRY_REALLOC(g_apikey.keys, new_cap * sizeof(*g_apikey.keys));
        if (!new_keys) {
            airy_mtx_unlock(&g_apikey.lock);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        g_apikey.keys = new_keys;
        g_apikey.capacity = new_cap;
    }

    g_apikey.keys[g_apikey.config.key_count++] = AIRY_STRDUP(new_key);
    airy_mtx_unlock(&g_apikey.lock);

    SVC_LOG_INFO("New API Key added (total=%zu)", g_apikey.config.key_count);
    return AUTH_SUCCESS;
}

int auth_apikey_remove(const char *key)
{
    if (!g_apikey.initialized || !key) {
        return AUTH_APIKEY_INVALID;
    }

    airy_mtx_lock(&g_apikey.lock);

    for (size_t i = 0; i < g_apikey.config.key_count; i++) {
        if (g_apikey.keys[i] && strcmp(key, g_apikey.keys[i]) == 0) {
            AIRY_FREE(g_apikey.keys[i]);
            g_apikey.keys[i] = NULL;

            for (size_t j = i; j < g_apikey.config.key_count - 1; j++) {
                g_apikey.keys[j] = g_apikey.keys[j + 1];
            }
            g_apikey.keys[g_apikey.config.key_count - 1] = NULL;
            g_apikey.config.key_count--;

            airy_mtx_unlock(&g_apikey.lock);
            SVC_LOG_INFO("API Key removed (remaining=%zu)", g_apikey.config.key_count);
            return AUTH_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_apikey.lock);
    return AUTH_APIKEY_INVALID;
}

void auth_apikey_cleanup(void)
{
    if (g_apikey.initialized) {
        airy_mtx_lock(&g_apikey.lock);
        if (g_apikey.keys) {
            for (size_t i = 0; i < g_apikey.config.key_count; i++) {
                AIRY_FREE(g_apikey.keys[i]);
            }
            AIRY_FREE(g_apikey.keys);
            g_apikey.keys = NULL;
        }
        g_apikey.config.key_count = 0;
        airy_mtx_unlock(&g_apikey.lock);
        airy_mtx_destroy(&g_apikey.lock);
        g_apikey.initialized = 0;
        SVC_LOG_INFO("API Key verification module cleaned up");
    }
}
