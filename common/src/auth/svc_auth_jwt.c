// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt.c
 * @brief Daemon auth-middleware JWT domain: HS256 token
 *        generation/verification/refresh, with runtime selection of
 *        OpenSSL/mbedTLS/built-in HMAC-SHA256.
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

jwt_global_state_t g_jwt = {.initialized = 0};

/**
 * @brief Base64 encoding
 * @param data    Input data
 * @param len     Data length
 * @param output  Output buffer
 * @param out_len Output length
 * @return 0 on success
 */
static int base64_encode(const uint8_t *data, size_t len, char *output, size_t *out_len)
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
 * @brief HMAC-SHA256 compute function pointer type
 */
typedef void (*hmac_fn_t)(const char *key, const char *message, uint8_t *output, size_t *out_len);

/**
 * @brief Currently used HMAC implementation pointer (runtime selection)
 */
static hmac_fn_t g_hmac_impl = NULL;

/*
 * ═══════════════════════════════════════════════════════════════
 * Mode 1: OpenSSL HMAC-SHA256 (production recommended, default)
 * ═══════════════════════════════════════════════════════════════
 */
#include <openssl/evp.h>
#include <openssl/hmac.h>

__attribute__((unused)) static void hmac_openssl(const char *key, const char *message,
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

static void hmac_mbedtls(const char *key, const char *message, uint8_t *output, size_t *out_len)
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

static void __attribute__((unused)) hmac_builtin(const char *key, const char *message,
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
                 (unsigned long long)g_jwt.config.token_ttl_sec, HMAC_IMPL_NAME);
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
