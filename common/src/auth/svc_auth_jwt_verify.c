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

    static const int b64_decode_table[256] = {
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,
        ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13,
        ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20,
        ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27,
        ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
        ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41,
        ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48,
        ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
        ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62,
        ['/'] = 63};

    const char *dot1 = strchr(token, '.');
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2) {
        result->error_message = "Invalid token format";
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t payload_len = (size_t)(dot2 - dot1 - 1);
    char *payload_b64 = (char *)AIRY_MALLOC(payload_len + 1);
    if (!payload_b64) {
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }
    __builtin_memcpy(payload_b64, dot1 + 1, payload_len);
    payload_b64[payload_len] = '\0';

    for (size_t i = 0; i < payload_len; i++) {
        if (payload_b64[i] == '-')
            payload_b64[i] = '+';
        else if (payload_b64[i] == '_')
            payload_b64[i] = '/';
    }

    size_t pad = (4 - (payload_len % 4)) % 4;
    size_t b64_total_len = payload_len + pad;
    char *payload_padded = (char *)AIRY_MALLOC(b64_total_len + 1);
    if (!payload_padded) {
        AIRY_FREE(payload_b64);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }
    __builtin_memcpy(payload_padded, payload_b64, payload_len);
    for (size_t i = 0; i < pad; i++)
        payload_padded[payload_len + i] = '=';
    payload_padded[b64_total_len] = '\0';
    AIRY_FREE(payload_b64);
    payload_b64 = NULL;

    size_t decoded_max = (b64_total_len / 4) * 3 + 4;
    unsigned char *payload_decoded = (unsigned char *)AIRY_MALLOC(decoded_max);
    if (!payload_decoded) {
        AIRY_FREE(payload_padded);
        airy_mtx_unlock(&g_jwt.lock);
        return AUTH_TOKEN_INVALID;
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < b64_total_len; i += 4) {
        int a = b64_decode_table[(unsigned char)payload_padded[i]];
        int b = b64_decode_table[(unsigned char)payload_padded[i + 1]];
        int c = (i + 2 < b64_total_len && payload_padded[i + 2] != '=') ?
                    b64_decode_table[(unsigned char)payload_padded[i + 2]] :
                    0;
        int d = (i + 3 < b64_total_len && payload_padded[i + 3] != '=') ?
                    b64_decode_table[(unsigned char)payload_padded[i + 3]] :
                    0;

        payload_decoded[out_idx++] = (unsigned char)((a << 2) | (b >> 4));
        if (i + 2 < b64_total_len && payload_padded[i + 2] != '=')
            payload_decoded[out_idx++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
        if (i + 3 < b64_total_len && payload_padded[i + 3] != '=')
            payload_decoded[out_idx++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    payload_decoded[out_idx] = '\0';
    AIRY_FREE(payload_padded);
    payload_padded = NULL;

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
        char *sig_b64 = (char *)AIRY_MALLOC(sig_b64_len + 1);
        if (!sig_b64) {
            AIRY_FREE(sig_input);
            result->error_message = "Memory allocation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }
        __builtin_memcpy(sig_b64, dot2 + 1, sig_b64_len);
        sig_b64[sig_b64_len] = '\0';

        for (size_t i = 0; i < sig_b64_len; i++) {
            if (sig_b64[i] == '-')
                sig_b64[i] = '+';
            else if (sig_b64[i] == '_')
                sig_b64[i] = '/';
        }

        size_t expected_sig_len = 32;
        uint8_t computed_hmac[32] = {0};
        g_hmac_impl(g_jwt.config.secret, sig_input, computed_hmac, &expected_sig_len);
        if (expected_sig_len == 0) {
            AIRY_FREE(sig_input);
            AIRY_FREE(sig_b64);
            result->error_message = "HMAC computation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }

        size_t sig_padded_len = sig_b64_len + ((4 - (sig_b64_len % 4)) % 4);
        char *sig_padded = (char *)AIRY_MALLOC(sig_padded_len + 1);
        if (!sig_padded) {
            AIRY_FREE(sig_input);
            AIRY_FREE(sig_b64);
            result->error_message = "Memory allocation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }
        __builtin_memcpy(sig_padded, sig_b64, sig_b64_len);
        size_t sig_pad = (4 - (sig_b64_len % 4)) % 4;
        for (size_t i = 0; i < sig_pad; i++)
            sig_padded[sig_b64_len + i] = '=';
        sig_padded[sig_padded_len] = '\0';

        unsigned char *provided_sig = (unsigned char *)AIRY_MALLOC(32);
        if (!provided_sig) {
            AIRY_FREE(sig_input);
            AIRY_FREE(sig_b64);
            AIRY_FREE(sig_padded);
            result->error_message = "Memory allocation failed";

            airy_mtx_unlock(&g_jwt.lock);
            return AUTH_TOKEN_INVALID;
        }
        __builtin_memset(provided_sig, 0, 32);

        size_t prov_idx = 0;
        for (size_t i = 0; i < sig_padded_len; i += 4) {
            int sa = b64_decode_table[(unsigned char)sig_padded[i]];
            int sb = b64_decode_table[(unsigned char)sig_padded[i + 1]];
            int sc = (i + 2 < sig_padded_len && sig_padded[i + 2] != '=') ?
                         b64_decode_table[(unsigned char)sig_padded[i + 2]] :
                         0;
            int sd = (i + 3 < sig_padded_len && sig_padded[i + 3] != '=') ?
                         b64_decode_table[(unsigned char)sig_padded[i + 3]] :
                         0;
            provided_sig[prov_idx++] = (unsigned char)((sa << 2) | (sb >> 4));
            if (i + 2 < sig_padded_len && sig_padded[i + 2] != '=')
                provided_sig[prov_idx++] = (unsigned char)(((sb & 0x0F) << 4) | (sc >> 2));
            if (i + 3 < sig_padded_len && sig_padded[i + 3] != '=')
                provided_sig[prov_idx++] = (unsigned char)(((sc & 0x03) << 6) | sd);
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
        AIRY_FREE(sig_b64);
        sig_b64 = NULL;
        AIRY_FREE(sig_padded);
        sig_padded = NULL;
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
