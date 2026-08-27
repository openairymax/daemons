// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_jwt_internal.h
 * @brief JWT 认证域拆分后的跨文件共享声明。
 *        svc_auth_jwt.c 按功能域拆分为 生命周期/令牌生成、编解码与
 *        HMAC 实现、令牌验证 三个域后，各域共用本头声明。
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_AUTH_JWT_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_SVC_AUTH_JWT_INTERNAL_H

#include "svc_auth_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HMAC-SHA256 计算函数指针类型（运行时实现选择）
 */
typedef void (*jwt_hmac_fn_t)(const char *key, const char *message, uint8_t *output,
                              size_t *out_len);

/**
 * @brief 当前选定的 HMAC 实现
 * @note 定义于 svc_auth_jwt_crypto.c，由 auth_jwt_init() 在初始化时选择
 */
extern jwt_hmac_fn_t g_hmac_impl;

/**
 * @brief Base64 编码原语（crypto 域，令牌生成/验证共用）
 */
int base64_encode(const uint8_t *data, size_t len, char *output, size_t *out_len);

/**
 * @brief 返回编译期选定的 HMAC 实现名（日志用途）
 */
const char *jwt_hmac_impl_name(void);

/**
 * @brief 三套 HMAC-SHA256 实现（crypto 域按编译分支定义，init 时选择其一）
 * @note 仅当前编译分支对应的实现会被定义
 */
void hmac_openssl(const char *key, const char *message, uint8_t *output, size_t *out_len);
void hmac_mbedtls(const char *key, const char *message, uint8_t *output, size_t *out_len);
void hmac_builtin(const char *key, const char *message, uint8_t *output, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_SVC_AUTH_JWT_INTERNAL_H */
