/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_auth.h
 * @brief Daemon 服务层认证中间件 - JWT/API Key/速率限制
 *
 * 设计原则 (遵循 ARCHITECTURAL_PRINCIPLES.md):
 * - E-1 安全内生: 默认安全，所有请求必须验证
 * - E-4 跨平台一致性: Windows/Linux/macOS 统一实现
 * - E-5 命名语义化: 清晰的 API 命名
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


/**
 * @brief JWT 认证配置结构体
 */
typedef struct jwt_config {
    const char *secret;
    size_t secret_len;
    uint64_t token_ttl_sec;
    uint64_t refresh_threshold_sec;
    const char *issuer;
} jwt_config_t;


/**
 * @brief API Key 验证配置结构体
 */
typedef struct apikey_config {
    const char **allowed_keys;
    size_t key_count;
    bool enable_key_rotation;
} apikey_config_t;


/**
 * @brief 速率限制器配置结构体
 */
typedef struct rate_limit_config {
    uint32_t requests_per_sec;
    uint32_t burst_size;
    size_t max_clients;
} rate_limit_config_t;


/**
 * @brief 认证结果上下文
 */
typedef struct auth_result {
    int status;
    const char *error_message;
    const char *subject;
    const char *role;
    int64_t expires_at;
} auth_result_t;


/**
 * @brief 初始化 JWT 认证模块
 * @param config JWT 配置
 * @return 0 成功，非0 失败
 *
 * @note 必须在服务启动时调用一次
 */
int auth_jwt_init(const jwt_config_t *config);

/**
 * @brief 生成 JWT Token
 * @param subject 主体标识（用户ID或Agent ID）
 * @param role 角色（admin/user/agent）
 * @param out_token 输出的 Token 字符串（需调用者释放）
 * @return 0 成功，非0 失败
 *
 * @ownership out_token: 调用者负责释放内存
 */
int auth_jwt_generate_token(const char *subject, const char *role, char **out_token);

/**
 * @brief 验证 JWT Token
 * @param token 待验证的 Token
 * @param result 输出验证结果
 * @return 0 验证成功，非0 失败
 *
 * @note 结果中的指针指向内部缓冲区，无需释放
 */
int auth_jwt_verify_token(const char *token, auth_result_t *result);

/**
 * @brief 刷新 JWT Token
 * @param old_token 旧 Token
 * @param out_new_token 新 Token（需调用者释放）
 * @return 0 成功，非0 失败
 */
int auth_jwt_refresh_token(const char *old_token, char **out_new_token);

/**
 * @brief 清理 JWT 模块资源
 */
void auth_jwt_cleanup(void);


/**
 * @brief 初始化 API Key 验证模块
 * @param config API Key 配置
 * @return 0 成功，非0 失败
 */
int auth_apikey_init(const apikey_config_t *config);

/**
 * @brief 验证 API Key
 * @param api_key 待验证的 Key
 * @param result 输出验证结果
 * @return 0 验证成功，非0 失败
 */
int auth_apikey_verify(const char *api_key, auth_result_t *result);

/**
 * @brief 动态添加允许的 API Key
 * @param new_key 新的 Key
 * @return 0 成功，非0 失败
 */
int auth_apikey_add(const char *new_key);

/**
 * @brief 移除 API Key
 * @param key 要移除的 Key
 * @return 0 成功，非0 失败
 */
int auth_apikey_remove(const char *key);

/**
 * @brief 清理 API Key 模块资源
 */
void auth_apikey_cleanup(void);


/**
 * @brief 初始化速率限制器
 * @param config 速率限制配置
 * @return 0 成功，非0 失败
 */
int auth_ratelimit_init(const rate_limit_config_t *config);

/**
 * @brief 检查是否允许请求
 * @param client_id 客户端标识（IP地址或连接ID）
 * @return 0 允许，AUTH_RATE_LIMIT_EXCEEDED 如果超限
 */
int auth_ratelimit_check(const char *client_id);

/**
 * @brief 重置客户端速率限制计数器
 * @param client_id 客户端标识
 * @return 0 成功
 */
int auth_ratelimit_reset(const char *client_id);

/**
 * @brief 获取当前速率限制统计信息
 * @param client_id 客户端标识
 * @param remaining 剩余可用请求数
 * @param reset_time 重置时间戳
 * @return 0 成功
 */
int auth_ratelimit_get_stats(const char *client_id, uint32_t *remaining, int64_t *reset_time);

/**
 * @brief 清理速率限制器资源
 */
void auth_ratelimit_cleanup(void);


/**
 * @brief 认证配置（统一初始化）
 */
typedef struct auth_config {
    jwt_config_t jwt;
    apikey_config_t apikey;
    rate_limit_config_t ratelimit;
    bool enable_jwt;
    bool enable_apikey;
    bool enable_ratelimit;
} auth_config_t;

/**
 * @brief 初始化所有认证模块
 * @param config 统一认证配置
 * @return 0 成功，非0 失败
 */
int auth_init(const auth_config_t *config);

/**
 * @brief 执行完整认证流程
 * @param auth_header Authorization 头部值（Bearer token 或 ApiKey xxx）
 * @param client_id 客户端标识
 * @param result 输出认证结果
 * @return 0 认证成功，非0 失败
 *
 * @details
 * 自动按顺序执行:
 * 1. 速率限制检查
 * 2. JWT 或 API Key 验证
 *
 * 支持两种格式:
 * - Bearer <jwt_token>
 * - ApiKey <api_key>
 */
int auth_authenticate(const char *auth_header, const char *client_id, auth_result_t *result);

/**
 * @brief 清理所有认证模块
 */
void auth_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_AUTH_H */
