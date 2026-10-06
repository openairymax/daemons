// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt_crypto.c
 * @brief JWT 认证域-编解码/HMAC 原语域：Base64 编解码与 HMAC-SHA256
 *        原语。HMAC 后端为 OpenSSL，由项目权威能力宏 AIRY_HAS_OPENSSL
 *        门控；未检出 OpenSSL 时由 auth_jwt_init() fail-closed 拒绝初始化。
 *        自 svc_auth_jwt.c 按功能域拆分，无外部 API 变化。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_auth_jwt_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Base64 encoding
 * @param data    Input data
 * @param len     Data length
 * @param output  Output buffer
 * @param out_len Output length
 * @return 0 on success
 */
int base64_encode(const uint8_t *data, size_t len, char *output, size_t *out_len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!data || !output || !out_len || len == 0) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "base64_encode: null parameter");
    }

    size_t needed = ((len + 2) / 3) * 4;
    if (*out_len < needed + 1) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "base64_encode: output buffer too small");
    }

    size_t i = 0, j = 0;
    uint8_t arr3[3] = {0}, arr4[4] = {0};

    while (i < len) {
        size_t group_start = i;
        arr3[0] = (i < len) ? data[i++] : 0;
        arr3[1] = (i < len) ? data[i++] : 0;
        arr3[2] = (i < len) ? data[i++] : 0;
        size_t consumed = i - group_start;

        arr4[0] = (arr3[0] & 0xFC) >> 2;
        arr4[1] = ((arr3[0] & 0x03) << 4) | ((arr3[1] & 0xF0) >> 4);
        arr4[2] = ((arr3[1] & 0x0F) << 2) | ((arr3[2] & 0xC0) >> 6);
        arr4[3] = arr3[2] & 0x3F;

        output[j++] = table[arr4[0]];
        output[j++] = table[arr4[1]];
        output[j++] = (consumed > 1) ? table[arr4[2]] : '=';
        output[j++] = (consumed > 2) ? table[arr4[3]] : '=';
    }

    output[j] = '\0';
    *out_len = j;

    return AIRY_SUCCESS;
}

/**
 * @brief Base64url 解码原语（crypto 域，令牌验证共用）
 *
 * 宽容语义与既有表驱动实现一致：非法字符按 0 解码（后续 HMAC 校验
 * 兜底拒绝），URL 安全字母表（-_）原地归一。输出缓冲区按解码长度
 * 分配并 NUL 终止，调用方负责 AIRY_FREE。
 */
int b64url_decode(const char *input, size_t in_len, unsigned char **out, size_t *out_len)
{
    static const int8_t table[256] = {
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

    *out = NULL;
    *out_len = 0;
    if (!input || !out || !out_len || in_len == 0)
        return AIRY_ERR_INVALID_PARAM;

    char *norm = (char *)AIRY_MALLOC(in_len + 4);
    if (!norm)
        return AIRY_ERR_INVALID_PARAM;
    for (size_t i = 0; i < in_len; i++) {
        char c = input[i];
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        norm[i] = c;
    }
    size_t pad = (4 - (in_len % 4)) % 4;
    for (size_t i = 0; i < pad; i++)
        norm[in_len + i] = '=';
    size_t total = in_len + pad;
    norm[total] = '\0';

    unsigned char *decoded = (unsigned char *)AIRY_MALLOC((total / 4) * 3 + 1);
    if (!decoded) {
        AIRY_FREE(norm);
        return AIRY_ERR_INVALID_PARAM;
    }

    size_t j = 0;
    for (size_t i = 0; i < total; i += 4) {
        int a = table[(unsigned char)norm[i]];
        int b = table[(unsigned char)norm[i + 1]];
        int c = (i + 2 < total && norm[i + 2] != '=') ? table[(unsigned char)norm[i + 2]] : 0;
        int d = (i + 3 < total && norm[i + 3] != '=') ? table[(unsigned char)norm[i + 3]] : 0;
        decoded[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (i + 2 < total && norm[i + 2] != '=')
            decoded[j++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
        if (i + 3 < total && norm[i + 3] != '=')
            decoded[j++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    decoded[j] = '\0';
    AIRY_FREE(norm);

    *out = decoded;
    *out_len = j;
    return AIRY_SUCCESS;
}

/**
 * @brief Currently used HMAC implementation pointer (runtime selection)
 */
jwt_hmac_fn_t g_hmac_impl = NULL;

/*
 * ═══════════════════════════════════════════════════════════════
 * HMAC-SHA256 后端：OpenSSL
 *
 * 由项目权威能力宏 AIRY_HAS_OPENSSL 门控（根 CMakeLists.txt 定义）。
 * 未检出 OpenSSL 时，JWT 模块在 auth_jwt_init() 处 fail-closed 拒绝
 * 初始化，而非退化为自研非加密安全哈希——安全关键路径不得使用降级
 * 或自研实现。
 * ═══════════════════════════════════════════════════════════════
 */
#ifdef AIRY_HAS_OPENSSL

#include <openssl/evp.h>
#include <openssl/hmac.h>

void hmac_openssl(const char *key, const char *message, uint8_t *output, size_t *out_len)
{
    unsigned int len = 0;
    unsigned int max_len = (unsigned int)(*out_len);
    if (HMAC(EVP_sha256(), (const unsigned char *)key, (int)strlen(key),
             (const unsigned char *)message, strlen(message), output, &len) == NULL) {
        *out_len = 0;
        return;
    }
    *out_len = (size_t)(len < max_len ? len : max_len);
}

#define HMAC_IMPL_NAME "OpenSSL"

#endif /* AIRY_HAS_OPENSSL */

const char *jwt_hmac_impl_name(void)
{
#ifdef HMAC_IMPL_NAME
    return HMAC_IMPL_NAME;
#else
    return "none";
#endif
}
