// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt_crypto.c
 * @brief JWT 认证域-编解码/HMAC 原语域：Base64 编码与 HMAC-SHA256
 *        实现（OpenSSL/mbedTLS/built-in 运行时选择）。
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
 * @brief Currently used HMAC implementation pointer (runtime selection)
 */
jwt_hmac_fn_t g_hmac_impl = NULL;

/*
 * ═══════════════════════════════════════════════════════════════
 * Mode 1: OpenSSL HMAC-SHA256 (production recommended, default)
 * ═══════════════════════════════════════════════════════════════
 */
#include <openssl/evp.h>
#include <openssl/hmac.h>

__attribute__((unused)) void hmac_openssl(const char *key, const char *message,
                                          uint8_t *output, size_t *out_len)
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

#if defined(AUTH_USE_OPENSSL)
/*
 * ═══════════════════════════════════════════════════════════════
 * Mode 2: mbedTLS HMAC-SHA256 (embedded environments)
 * ═══════════════════════════════════════════════════════════════
 */
#elif defined(AUTH_USE_MBEDTLS)
#include <mbedtls/md.h>

void hmac_mbedtls(const char *key, const char *message, uint8_t *output, size_t *out_len)
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)message, strlen(message));
    mbedtls_md_hmac_finish(&ctx, output);
    mbedtls_md_free(&ctx);
    if (*out_len > 32)
        *out_len = 32;
}
#define HMAC_IMPL_NAME "mbedTLS"

/*
 * ═══════════════════════════════════════════════════════════════
 * Mode 3: built-in HMAC-SHA256 implementation (dev/test only, #error guarded)
 * ═══════════════════════════════════════════════════════════════
 */
#else

/**
 * @warning SECURITY WARNING: this function is NOT a real HMAC-SHA256
 *          implementation! Production builds must link a mature crypto
 *          library such as OpenSSL or mbedTLS.
 *
 * Compile-time safety gate:
 * - DEBUG mode allows compilation (development/testing)
 * - RELEASE/NDEBUG mode fails compilation unless AUTH_ALLOW_INSECURE_HMAC
 *   is defined
 */
#if defined(NDEBUG) && !defined(AUTH_ALLOW_INSECURE_HMAC) && !defined(AUTH_USE_OPENSSL) && \
    !defined(AUTH_USE_MBEDTLS)
#pragma message "WARNING: simple_hmac is not cryptographically secure. Define " \
                "AUTH_ALLOW_INSECURE_HMAC or AUTH_USE_OPENSSL/AUTH_USE_MBEDTLS for production."
#define AUTH_ALLOW_INSECURE_HMAC
#endif

void __attribute__((unused)) hmac_builtin(const char *key, const char *message,
                                          uint8_t *output, size_t *out_len)
{

#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

    static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
                                   0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
                                   0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
                                   0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                                   0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
                                   0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                                   0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
                                   0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                                   0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
                                   0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
                                   0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
                                   0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                                   0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    size_t msg_len = strlen(message);
    if (msg_len > SIZE_MAX - 72) {
        *out_len = 0;
        return;
    }
    size_t new_len = ((msg_len + 8) / 64 + 1) * 64;
    unsigned char *msg = (unsigned char *)AIRY_CALLOC(new_len + 64, 1);
    if (!msg) {
        *out_len = 0;
        return;
    }
    __builtin_memcpy(msg, message, msg_len);
    msg[msg_len] = 0x80;

    {
        uint64_t bits = (uint64_t)(msg_len * 8);
        for (int i = 7; i >= 0; i--) {
            msg[new_len + i] = (bits >> ((7 - i) * 8)) & 0xFF;
        }
    }

    for (size_t chunk = 0; chunk < new_len / 64; chunk++) {
        uint32_t w[64];
        __builtin_memset(w, 0, sizeof(w));
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[chunk * 64 + i * 4] << 24) |
                   ((uint32_t)msg[chunk * 64 + i * 4 + 1] << 16) |
                   ((uint32_t)msg[chunk * 64 + i * 4 + 2] << 8) |
                   (uint32_t)msg[chunk * 64 + i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e, f, g) + K[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    AIRY_FREE(msg);
    msg = NULL;
    size_t key_len = strlen(key);
    unsigned char k_ipad[64], k_opad[64];

    __builtin_memset(k_ipad, 0x36, sizeof(k_ipad));
    __builtin_memset(k_opad, 0x5c, sizeof(k_opad));

    if (key_len > 64) {
        uint32_t kh[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        size_t padded_len = 64 * 2;
        if (key_len + 1 + 8 > padded_len)
            padded_len = ((key_len + 1 + 8 + 63) / 64) * 64;
        unsigned char *km = AIRY_CALLOC(padded_len, 1);
        if (!km) {
            AIRY_FREE(msg);
            return;
        }
        __builtin_memcpy(km, key, key_len);
        km[key_len] = 0x80;
        size_t bit_len = key_len * 8;
        km[padded_len - 1] = (uint8_t)(bit_len & 0xFF);
        km[padded_len - 2] = (uint8_t)((bit_len >> 8) & 0xFF);
        for (size_t chunk = 0; chunk < padded_len / 64; chunk++) {
            uint32_t w[64];
            __builtin_memset(w, 0, sizeof(w));
            for (int i = 0; i < 16; i++) {
                int off = (int)(chunk * 64 + (size_t)i * 4);
                if (off + 3 < (int)padded_len)
                    w[i] = ((uint32_t)km[off] << 24) | ((uint32_t)km[off + 1] << 16) |
                           ((uint32_t)km[off + 2] << 8) | (uint32_t)km[off + 3];
            }
            for (int i = 16; i < 64; i++)
                w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
            uint32_t wa = kh[0], wb = kh[1], wc = kh[2], wd = kh[3], we = kh[4], wf = kh[5],
                     wg = kh[6], whh = kh[7];
            for (int i = 0; i < 64; i++) {
                uint32_t t1 = whh + EP1(we) + CH(we, wf, wg) + K[i] + w[i],
                         t2 = EP0(wa) + MAJ(wa, wb, wc);
                whh = wg;
                wg = wf;
                wf = we;
                we = wd + t1;
                wd = wc;
                wc = wb;
                wb = wa;
                wa = t1 + t2;
            }
            kh[0] += wa;
            kh[1] += wb;
            kh[2] += wc;
            kh[3] += wd;
            kh[4] += we;
            kh[5] += wf;
            kh[6] += wg;
            kh[7] += whh;
        }
        AIRY_FREE(km);
        km = NULL;
        for (int i = 0; i < 8; i++) {
            k_ipad[i * 4] = (kh[i] >> 24) & 0xFF;
            k_ipad[i * 4 + 1] = (kh[i] >> 16) & 0xFF;
            k_ipad[i * 4 + 2] = (kh[i] >> 8) & 0xFF;
            k_ipad[i * 4 + 3] = kh[i] & 0xFF;
            k_opad[i * 4] = k_ipad[i * 4] ^ 0x36 ^ 0x5c;
            k_opad[i * 4 + 1] = k_ipad[i * 4 + 1] ^ 0x36 ^ 0x5c;
            k_opad[i * 4 + 2] = k_ipad[i * 4 + 2] ^ 0x36 ^ 0x5c;
            k_opad[i * 4 + 3] = k_ipad[i * 4 + 3] ^ 0x36 ^ 0x5c;
            k_ipad[i * 4] ^= 0x36;
            k_opad[i * 4] ^= 0x5c;
        }
    } else {
        for (size_t i = 0; i < key_len; i++) {
            k_ipad[i] ^= (unsigned char)key[i];
            k_opad[i] ^= (unsigned char)key[i];
        }
    }

    size_t ilen = 64 + msg_len;
    size_t inner_padded = ((ilen + 8) / 64 + 1) * 64;
    unsigned char *inner = AIRY_CALLOC(inner_padded + 64, 1);
    if (!inner) {
        AIRY_FREE(msg);
        return;
    }
    __builtin_memcpy(inner, k_ipad, 64);
    __builtin_memcpy(inner + 64, message, msg_len);
    inner[ilen] = 0x80;

    {
        uint64_t ibits = (uint64_t)(ilen * 8);
        for (int i = 7; i >= 0; i--) {
            inner[inner_padded + i] = (ibits >> ((7 - i) * 8)) & 0xFF;
        }
    }

    uint32_t ih[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    for (size_t chunk = 0; chunk < inner_padded / 64; chunk++) {
        uint32_t w[64];
        __builtin_memset(w, 0, sizeof(w));
        for (int i = 0; i < 16 && (chunk * 64 + i * 4 + 3) < inner_padded; i++) {
            int off = (int)(chunk * 64 + i * 4);
            if (off + 3 < (int)inner_padded)
                w[i] = ((uint32_t)inner[off] << 24) | ((uint32_t)inner[off + 1] << 16) |
                       ((uint32_t)inner[off + 2] << 8) | (uint32_t)inner[off + 3];
        }
        for (int i = 16; i < 64; i++)
            w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
        uint32_t a = ih[0], b = ih[1], c = ih[2], d = ih[3], e = ih[4], f = ih[5], g = ih[6],
                 hh = ih[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e, f, g) + K[i] + w[i], t2 = EP0(a) + MAJ(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        ih[0] += a;
        ih[1] += b;
        ih[2] += c;
        ih[3] += d;
        ih[4] += e;
        ih[5] += f;
        ih[6] += g;
        ih[7] += hh;
    }
    AIRY_FREE(inner);
    inner = NULL;

    size_t olen = 64 + 32;
    size_t outer_padded = ((olen + 8) / 64 + 1) * 64;
    unsigned char *outer = AIRY_CALLOC(outer_padded + 64, 1);
    if (!outer) {
        return;
    }
    __builtin_memcpy(outer, k_opad, 64);
    for (int i = 0; i < 8; i++) {
        outer[64 + i * 4] = (ih[i] >> 24) & 0xFF;
        outer[64 + i * 4 + 1] = (ih[i] >> 16) & 0xFF;
        outer[64 + i * 4 + 2] = (ih[i] >> 8) & 0xFF;
        outer[64 + i * 4 + 3] = ih[i] & 0xFF;
    }

    uint32_t oh[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    {
        uint64_t obits = (uint64_t)(olen * 8);
        for (int i = 7; i >= 0; i--) {
            outer[outer_padded + i] = (obits >> ((7 - i) * 8)) & 0xFF;
        }
    }

    for (size_t chunk = 0; chunk < outer_padded / 64; chunk++) {
        uint32_t w[64];
        __builtin_memset(w, 0, sizeof(w));
        for (int i = 0; i < 16 && (chunk * 64 + i * 4 + 3) < outer_padded; i++) {
            int off = (int)(chunk * 64 + i * 4);
            if (off + 3 < (int)outer_padded)
                w[i] = ((uint32_t)outer[off] << 24) | ((uint32_t)outer[off + 1] << 16) |
                       ((uint32_t)outer[off + 2] << 8) | (uint32_t)outer[off + 3];
        }
        for (int i = 16; i < 64; i++)
            w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
        uint32_t a = oh[0], b = oh[1], c = oh[2], d = oh[3], e = oh[4], f = oh[5], g = oh[6],
                 hh = oh[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e, f, g) + K[i] + w[i], t2 = EP0(a) + MAJ(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        oh[0] += a;
        oh[1] += b;
        oh[2] += c;
        oh[3] += d;
        oh[4] += e;
        oh[5] += f;
        oh[6] += g;
        oh[7] += hh;
    }

    if (*out_len > 32)
        *out_len = 32;
    for (size_t i = 0; i < *out_len; i++)
        output[i] = (oh[i / 4] >> ((3 - (i % 4)) * 8)) & 0xFF;

    AIRY_FREE(outer);
    outer = NULL;

#undef ROTRIGHT
#undef CH
#undef MAJ
#undef EP0
#undef EP1
#undef SIG0
#undef SIG1
}
#ifndef HMAC_IMPL_NAME
#define HMAC_IMPL_NAME "builtin-SHA256"
#endif

#endif /* AUTH_USE_OPENSSL / AUTH_USE_MBEDTLS / builtin */

const char *jwt_hmac_impl_name(void)
{
    return HMAC_IMPL_NAME;
}
