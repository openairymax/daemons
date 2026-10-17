/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_auth.h: Provider 域鉴权头声明（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_AUTH_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_AUTH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROVIDER_AUTH_NONE = 0,
    PROVIDER_AUTH_BEARER,
    PROVIDER_AUTH_X_API_KEY,
} provider_auth_kind_t;

/* 适配层只声明"用哪种鉴权、附加哪些静态头"，拼装、密钥擦除与头链释放
 * 全部由 core/http.c 兜住。 */
typedef struct {
    provider_auth_kind_t auth;
    const char *const *extra;
    size_t extra_count;
} provider_header_spec_t;

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_AUTH_H */
