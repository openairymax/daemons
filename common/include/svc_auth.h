/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_auth.h
 * @brief Daemon service-layer auth middleware - JWT/API Key/rate limiting.
 *
 * Design principles (following ARCHITECTURAL_PRINCIPLES.md):
 * - E-1 security built-in: secure by default, all requests must be verified
 * - E-4 cross-platform consistency: uniform Windows/Linux/macOS implementation
 * - E-5 semantic naming: clear API naming
 */

#ifndef SVC_AUTH_H
#define SVC_AUTH_H

#include "daemon_errors.h"
#include "error.h"
#include "daemon_platform_ext.h"

#ifdef __cplusplus
extern "C" {
#endif


#define AUTH_SUCCESS AIRY_SUCCESS

#define AUTH_FAILED AIRY_ERR_PERMISSION_DENIED

#define AUTH_TOKEN_EXPIRED (AIRY_ERR_DAEMON_BASE + 0x30)

#define AUTH_TOKEN_INVALID (AIRY_ERR_DAEMON_BASE + 0x31)

#define AUTH_APIKEY_INVALID (AIRY_ERR_DAEMON_BASE + 0x32)

#define AUTH_RATE_LIMIT_EXCEEDED (AIRY_ERR_DAEMON_BASE + 0x33)

#define AUTH_MISSING_CREDENTIALS (AIRY_ERR_DAEMON_BASE + 0x34)


/** @brief JWT authentication config struct. */
typedef struct jwt_config {
    const char *secret;
    size_t secret_len;
    uint64_t token_ttl_sec;
    uint64_t refresh_threshold_sec;
    const char *issuer;
} jwt_config_t;


/** @brief API Key verification config struct. */
typedef struct apikey_config {
    const char **allowed_keys;
    size_t key_count;
    bool enable_key_rotation;
} apikey_config_t;


/** @brief Rate-limiter config struct. */
typedef struct rate_limit_config {
    uint32_t requests_per_sec;
    uint32_t burst_size;
    size_t max_clients;
} rate_limit_config_t;


/** @brief Authentication result context. */
typedef struct auth_result {
    int status;
    const char *error_message;
    const char *subject;
    const char *role;
    int64_t expires_at;
    /* 内嵌 subject/role 存储（t11-02）：verify 时拷贝到 result 自带缓冲，
     * subject/role 指向本结构内嵌存储，避免指向模块全局缓冲而被并发
     * 请求覆盖（此前 auth_apikey_verify/auth_jwt_verify 均把 result 字段
     * 指向 g_apikey.subject_buf / g_jwt.subject_buf，锁释放后即产生竞争）。
     * 大小与 svc_auth_internal.h 的 MAX_SUBJECT_SIZE/MAX_ROLE_SIZE 对齐。 */
    char subject_storage[256];
    char role_storage[64];
} auth_result_t;


/**
 * @brief Initialize the JWT auth module.
 * @param config JWT config
 * @return 0 on success, non-zero on failure
 *
 * @note Must be called once at service startup
 */
int auth_jwt_init(const jwt_config_t *config);

/**
 * @brief Generate a JWT token.
 * @param subject Subject (user ID or agent ID)
 * @param role Role (admin/user/agent)
 * @param out_token Output token string (caller frees)
 * @return 0 on success, non-zero on failure
 *
 * @ownership out_token: caller frees the memory
 */
int auth_jwt_generate_token(const char *subject, const char *role, char **out_token);

/**
 * @brief Verify a JWT token.
 * @param token Token to verify
 * @param result Output verification result
 * @return 0 on success, non-zero on failure
 *
 * @note Pointers in the result reference internal buffers; no free needed
 */
int auth_jwt_verify_token(const char *token, auth_result_t *result);

/**
 * @brief Refresh a JWT token.
 * @param old_token Old token
 * @param out_new_token New token (caller frees)
 * @return 0 on success, non-zero on failure
 */
int auth_jwt_refresh_token(const char *old_token, char **out_new_token);

/** @brief Clean up JWT module resources. */
void auth_jwt_cleanup(void);


/**
 * @brief Initialize the API Key verification module.
 * @param config API Key config
 * @return 0 on success, non-zero on failure
 */
int auth_apikey_init(const apikey_config_t *config);

/**
 * @brief Verify an API key.
 * @param api_key Key to verify
 * @param result Output verification result
 * @return 0 on success, non-zero on failure
 */
int auth_apikey_verify(const char *api_key, auth_result_t *result);

/**
 * @brief Dynamically add an allowed API key.
 * @param new_key New key
 * @return 0 on success, non-zero on failure
 */
int auth_apikey_add(const char *new_key);

/**
 * @brief Remove an API key.
 * @param key Key to remove
 * @return 0 on success, non-zero on failure
 */
int auth_apikey_remove(const char *key);

/** @brief Clean up API Key module resources. */
void auth_apikey_cleanup(void);


/**
 * @brief Initialize the rate limiter.
 * @param config Rate-limit config
 * @return 0 on success, non-zero on failure
 */
int auth_ratelimit_init(const rate_limit_config_t *config);

/**
 * @brief Check whether a request is allowed.
 * @param client_id Client identifier (IP address or connection ID)
 * @return 0 allowed, AUTH_RATE_LIMIT_EXCEEDED if over the limit
 */
int auth_ratelimit_check(const char *client_id);

/**
 * @brief Reset a client's rate-limit counters.
 * @param client_id Client identifier
 * @return 0 on success
 */
int auth_ratelimit_reset(const char *client_id);

/**
 * @brief Get current rate-limit statistics.
 * @param client_id Client identifier
 * @param remaining Remaining allowed requests
 * @param reset_time Reset timestamp
 * @return 0 on success
 */
int auth_ratelimit_get_stats(const char *client_id, uint32_t *remaining, int64_t *reset_time);

/** @brief Clean up rate-limiter resources. */
void auth_ratelimit_cleanup(void);


/** @brief Auth config (unified init). */
typedef struct auth_config {
    jwt_config_t jwt;
    apikey_config_t apikey;
    rate_limit_config_t ratelimit;
    bool enable_jwt;
    bool enable_apikey;
    bool enable_ratelimit;
} auth_config_t;

/**
 * @brief Initialize all auth modules.
 * @param config Unified auth config
 * @return 0 on success, non-zero on failure
 */
int auth_init(const auth_config_t *config);

/**
 * @brief Run the full authentication flow.
 * @param auth_header Authorization header value (Bearer token or ApiKey xxx)
 * @param client_id Client identifier
 * @param result Output auth result
 * @return 0 on success, non-zero on failure
 *
 * @details
 * Runs automatically, in order:
 * 1. Rate-limit check
 * 2. JWT or API Key verification
 *
 * Supports two formats:
 * - Bearer <jwt_token>
 * - ApiKey <api_key>
 */
int auth_authenticate(const char *auth_header, const char *client_id, auth_result_t *result);

/** @brief Clean up all auth modules. */
void auth_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_AUTH_H */
