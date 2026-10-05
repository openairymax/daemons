// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt_verify.c
 * @brief JWT 认证域-令牌验证域：auth_jwt_verify_token() 的 token 结构
 *        解析、payload 提取与 HMAC 签名校验。
 *        自 svc_auth_jwt.c 按功能域拆分，无外部 API 变化。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_auth.h"
#include "svc_auth_internal.h"
#include "svc_auth_jwt_internal.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int auth_jwt_verify_token(const char *token, auth_result_t *result)
{
    if (!g_jwt.initialized || !token || !result) {
        return AUTH_TOKEN_INVALID;
    }

    airy_mtx_lock(&g_jwt.lock);

    __builtin_memset(result, 0, sizeof(auth_result_t));
    result->status = AUTH_FAILED;
    result->error_message = "Token verification failed";

    const char *dot1 = strchr(token, '.');
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2) {
        result->error_message = "Invalid token format";
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t payload_len = (size_t)(dot2 - dot1 - 1);
    unsigned char *payload_decoded = NULL;
    size_t payload_decoded_len = 0;
    if (b64url_decode(dot1 + 1, payload_len, &payload_decoded, &payload_decoded_len) !=
        AIRY_SUCCESS) {
        result->error_message = "Invalid token payload";
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    CJSON_PARSE_GUARD(payload, (const char *)payload_decoded, {
        AIRY_FREE(payload_decoded);
        payload_decoded = NULL;
        result->error_message = "Invalid token payload";
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    });
    AIRY_FREE(payload_decoded);
    payload_decoded = NULL;

    cJSON *sub = cJSON_GetObjectItem(payload, "sub");
    cJSON *role = cJSON_GetObjectItem(payload, "role");
    cJSON *exp = cJSON_GetObjectItem(payload, "exp");

    if (cJSON_IsString(sub)) {
        /* t11-02: 拷贝到 result 内嵌存储而非全局 g_jwt.subject_buf，
         * 避免锁释放后并发请求覆盖 subject */
        AIRY_STRNCPY_TERM(result->subject_storage, sub->valuestring,
                          sizeof(result->subject_storage));
        result->subject_storage[sizeof(result->subject_storage) - 1] = '\0';
        if (strlen(sub->valuestring) >= sizeof(result->subject_storage)) {
            SVC_LOG_WARN("JWT subject truncated to %zu chars: original length=%zu",
                         sizeof(result->subject_storage) - 1, strlen(sub->valuestring));
        }
        result->subject = result->subject_storage;
    }
    if (cJSON_IsString(role)) {
        AIRY_STRNCPY_TERM(result->role_storage, role->valuestring, sizeof(result->role_storage));
        result->role_storage[sizeof(result->role_storage) - 1] = '\0';
        result->role = result->role_storage;
    }

    if (cJSON_IsNumber(exp)) {
        time_t exp_time = (time_t)exp->valuedouble;
        time_t now = time(NULL);
        result->expires_at = (int64_t)exp_time * 1000;

        if (now > exp_time) {
            result->status = AUTH_TOKEN_EXPIRED;
            result->error_message = "Token has expired";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_EXPIRED;
        }
    }

    {
        size_t header_len = (size_t)(dot1 - token);
        size_t sig_input_len = header_len + 1 + payload_len;
        char *sig_input = (char *)AIRY_MALLOC(sig_input_len + 1);
        if (!sig_input) {
            result->error_message = "Memory allocation failed for signature verification";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }
        __builtin_memcpy(sig_input, token, header_len);
        sig_input[header_len] = '.';
        __builtin_memcpy(sig_input + header_len + 1, dot1 + 1, payload_len);
        sig_input[sig_input_len] = '\0';

        size_t sig_b64_len = strlen(dot2 + 1);

        size_t expected_sig_len = 32;
        uint8_t computed_hmac[32] = {0};
        g_hmac_impl(g_jwt.config.secret, sig_input, computed_hmac, &expected_sig_len);
        if (expected_sig_len == 0) {
            AIRY_FREE(sig_input);
            result->error_message = "HMAC computation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }

        unsigned char *provided_sig = NULL;
        size_t prov_idx = 0;
        if (b64url_decode(dot2 + 1, sig_b64_len, &provided_sig, &prov_idx) != AIRY_SUCCESS) {
            AIRY_FREE(sig_input);
            result->error_message = "Memory allocation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }

        int sig_match = 1;
        if (prov_idx < expected_sig_len) {
            sig_match = 0;
        } else {
            volatile const uint8_t *left = (volatile const uint8_t *)computed_hmac;
            volatile const uint8_t *right = (volatile const uint8_t *)provided_sig;
            uint8_t acc = 0;
            for (size_t i = 0; i < expected_sig_len; i++) {
                acc |= left[i] ^ right[i];
            }
            sig_match = (acc == 0);
        }

        AIRY_FREE(sig_input);
        sig_input = NULL;
        AIRY_FREE(provided_sig);
        provided_sig = NULL;

        if (!sig_match) {
            result->status = AUTH_FAILED;
            result->error_message = "Invalid token signature";

            SVC_LOG_WARN("JWT signature verification FAILED for token");
            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }
    }

    result->status = AUTH_SUCCESS;
    result->error_message = NULL;

    SVC_LOG_DEBUG("JWT token verified for subject=%s",
                  result->subject ? result->subject : "unknown");
    airy_mtx_unlock(&g_jwt.lock);
    return AUTH_SUCCESS;
}
