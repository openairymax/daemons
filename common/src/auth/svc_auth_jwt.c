// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt.c
 * @brief Daemon auth-middleware JWT 域-生命周期/令牌生成域：模块
 *        初始化/清理与 HS256 token 生成/刷新（HMAC 实现与 Base64
 *        原语见 svc_auth_jwt_crypto.c，令牌验证见 svc_auth_jwt_verify.c）。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_auth.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "svc_auth_internal.h"
#include "svc_auth_jwt_internal.h"

jwt_global_state_t g_jwt = {.initialized = 0};

int auth_jwt_init(const jwt_config_t *config)
{
    airy_mtx_lock(&g_jwt.lock);

    if (g_jwt.initialized) {
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_SUCCESS;
    }

    if (!config || !config->secret || config->secret_len == 0) {
        SVC_LOG_ERROR("JWT init: invalid config");
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    __builtin_memcpy(&g_jwt.config, config, sizeof(jwt_config_t));

    if (g_jwt.config.token_ttl_sec == 0)
        g_jwt.config.token_ttl_sec = DEFAULT_TOKEN_TTL;
    if (g_jwt.config.refresh_threshold_sec == 0)
        g_jwt.config.refresh_threshold_sec = DEFAULT_REFRESH_THRESHOLD;

#if defined(AUTH_USE_OPENSSL)
    g_hmac_impl = hmac_openssl;
#elif defined(AUTH_USE_MBEDTLS)
    g_hmac_impl = hmac_mbedtls;
#else
    g_hmac_impl = hmac_builtin;
#endif

    airy_mtx_init(&g_jwt.lock);
    g_jwt.initialized = 1;
    SVC_LOG_INFO("JWT authentication module initialized (TTL=%llu sec, HMAC=%s)",
                 (unsigned long long)g_jwt.config.token_ttl_sec, jwt_hmac_impl_name());
    airy_mtx_unlock(&g_jwt.lock);
    return AUTH_SUCCESS;
}

int auth_jwt_generate_token(const char *subject, const char *role, char **out_token)
{
    if (!g_jwt.initialized || !subject || !out_token) {
        return AUTH_TOKEN_INVALID;
    }

    airy_mtx_lock(&g_jwt.lock);

    if (strlen(subject) > MAX_SUBJECT_SIZE) {
        SVC_LOG_ERROR("JWT generate: subject too long");
        airy_mtx_unlock(&g_jwt.lock);
        AIRY_ERROR(AUTH_TOKEN_INVALID, "JWT generate: subject too long");
    }

    const char *header_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

    time_t now = time(NULL);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "iss",
                            g_jwt.config.issuer ? g_jwt.config.issuer : "agentrt-daemon");
    cJSON_AddStringToObject(payload, "sub", subject);
    cJSON_AddStringToObject(payload, "role", role ? role : "user");
    cJSON_AddNumberToObject(payload, "iat", (double)now);
    cJSON_AddNumberToObject(payload, "exp", (double)(now + g_jwt.config.token_ttl_sec));

    char *payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    if (!payload_json) {
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t payload_b64_size = strlen(payload_json) * 2 + 100;
    char *payload_b64 = (char *)AIRY_MALLOC(payload_b64_size);
    if (!payload_b64) {
        AIRY_FREE(payload_json);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    base64_encode((const uint8_t *)payload_json, strlen(payload_json), payload_b64,
                  &payload_b64_size);
    AIRY_FREE(payload_json);
    payload_json = NULL;

    size_t sign_input_size = strlen(header_b64) + 1 + payload_b64_size + 100;
    char *sign_input = (char *)AIRY_MALLOC(sign_input_size);
    if (!sign_input) {
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }
    snprintf(sign_input, sign_input_size, "%s.%s", header_b64, payload_b64);

    uint8_t hmac_output[32] = {0};
    size_t hmac_len = sizeof(hmac_output);
    g_hmac_impl(g_jwt.config.secret, sign_input, hmac_output, &hmac_len);
    if (hmac_len == 0) {
        AIRY_FREE(sign_input);
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t sig_b64_size = 128;
    char *sig_b64 = (char *)AIRY_MALLOC(sig_b64_size);
    if (!sig_b64) {
        AIRY_FREE(sign_input);
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }
    if (base64_encode(hmac_output, hmac_len, sig_b64, &sig_b64_size) != AIRY_SUCCESS) {
        AIRY_FREE(sign_input);
        AIRY_FREE(sig_b64);
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t token_size = sign_input_size + sig_b64_size + 10;
    *out_token = (char *)AIRY_MALLOC(token_size);
    if (!*out_token) {
        AIRY_FREE(sign_input);
        AIRY_FREE(sig_b64);
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }
    snprintf(*out_token, token_size, "%s.%s", sign_input, sig_b64);

    AIRY_FREE(sign_input);
    AIRY_FREE(payload_b64);
    AIRY_FREE(sig_b64);

    SVC_LOG_DEBUG("JWT token generated for subject=%s", subject);
    airy_mtx_unlock(&g_jwt.lock);
    return AUTH_SUCCESS;
}

int auth_jwt_refresh_token(const char *old_token, char **out_new_token)
{
    if (!old_token || !out_new_token)
        return AUTH_TOKEN_INVALID;

    auth_result_t result;
    int ret = auth_jwt_verify_token(old_token, &result);
    if (ret != AUTH_SUCCESS) {
        return ret;
    }

    return auth_jwt_generate_token(result.subject, result.role, out_new_token);
}

void auth_jwt_cleanup(void)
{
    airy_mtx_lock(&g_jwt.lock);
    if (g_jwt.initialized) {
        g_hmac_impl = NULL;
        g_jwt.initialized = 0;
        __builtin_memset(&g_jwt.config, 0, sizeof(jwt_config_t));
        __builtin_memset(g_jwt.subject_buf, 0, sizeof(g_jwt.subject_buf));
        __builtin_memset(g_jwt.role_buf, 0, sizeof(g_jwt.role_buf));
        SVC_LOG_INFO("JWT authentication module cleaned up");
    }
    airy_mtx_unlock(&g_jwt.lock);
    airy_mtx_destroy(&g_jwt.lock);
}
