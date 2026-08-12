// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file svc_auth.c
 * @brief Daemon service-layer auth middleware implementation.
 *
 * Implements:
 * - JWT token generation and verification (HS256)
 * - API Key verification and dynamic management
 * - Token-bucket rate limiter
 * - Unified authentication entry
 */

#include "daemon_defaults.h"
#include "svc_auth.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "svc_auth_internal.h"

int auth_init(const auth_config_t *config)
{
    int ret = 0;

    if (!config)
        return AUTH_FAILED;

    if (config->enable_jwt) {
        const jwt_config_t *jwt_cfg = &config->jwt;
        ret = auth_jwt_init(jwt_cfg);
        if (ret != AUTH_SUCCESS)
            return ret;
    }

    if (config->enable_apikey) {
        ret = auth_apikey_init(&config->apikey);
        if (ret != AUTH_SUCCESS) {
            auth_jwt_cleanup();
            return ret;
        }
    }

    if (config->enable_ratelimit) {
        ret = auth_ratelimit_init(&config->ratelimit);
        if (ret != AUTH_SUCCESS) {
            auth_apikey_cleanup();
            auth_jwt_cleanup();
            return ret;
        }
    }

    SVC_LOG_INFO("Authentication middleware initialized successfully");
    return AUTH_SUCCESS;
}

int auth_authenticate(const char *auth_header, const char *client_id, auth_result_t *result)
{
    if (!auth_header || !result) {
        return AUTH_MISSING_CREDENTIALS;
    }

    __builtin_memset(result, 0, sizeof(auth_result_t));

    if (g_ratelimit.initialized) {
        int rl_ret = auth_ratelimit_check(client_id);
        if (rl_ret != AUTH_SUCCESS) {
            result->status = AUTH_RATE_LIMIT_EXCEEDED;
            result->error_message = "Too many requests";
            return AUTH_RATE_LIMIT_EXCEEDED;
        }
    }

    if (strncmp(auth_header, BEARER_PREFIX, strlen(BEARER_PREFIX)) == 0) {
        /* Bearer Token (JWT) */
        if (!g_jwt.initialized) {
            result->status = AUTH_FAILED;
            result->error_message = "JWT not enabled";
            return AUTH_FAILED;
        }

        const char *token = auth_header + strlen(BEARER_PREFIX);
        return auth_jwt_verify_token(token, result);

    } else if (strncmp(auth_header, APIKEY_PREFIX, strlen(APIKEY_PREFIX)) == 0) {
        /* API Key */
        if (!g_apikey.initialized) {
            result->status = AUTH_FAILED;
            result->error_message = "API Key not enabled";
            return AUTH_FAILED;
        }

        const char *key = auth_header + strlen(APIKEY_PREFIX);
        return auth_apikey_verify(key, result);
    }

    result->status = AUTH_MISSING_CREDENTIALS;
    result->error_message = "Missing or invalid Authorization header";
    return AUTH_MISSING_CREDENTIALS;
}

void auth_cleanup(void)
{
    auth_jwt_cleanup();
    auth_apikey_cleanup();
    auth_ratelimit_cleanup();
    SVC_LOG_INFO("All authentication modules cleaned up");
}
// force rebuild
// v2
