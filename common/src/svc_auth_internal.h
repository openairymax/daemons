/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_auth_internal.h
 * @brief Daemon 服务层认证中间件内部跨文件共享声明（svc_auth.c 拆分后各域共用）
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_AUTH_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_SVC_AUTH_INTERNAL_H

#include "daemon_defaults.h"
#include "svc_auth.h"
#include "svc_logger.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TOKEN_SIZE 4096
#define MAX_SUBJECT_SIZE 256
#define MAX_ROLE_SIZE 64
#define MAX_APIKEY_SIZE 128
#define MAX_CLIENTS 1024

#define DEFAULT_TOKEN_TTL AIRY_DEFAULT_TOKEN_TTL_SEC
#define DEFAULT_REFRESH_THRESHOLD AIRY_DEFAULT_REFRESH_THRESHOLD
#define DEFAULT_RPS AIRY_DEFAULT_RPS_LIMIT
#define DEFAULT_BURST_SIZE AIRY_DEFAULT_BURST_SIZE
#define TOKEN_PREFIX "agentrt."
#define BEARER_PREFIX "Bearer "
#define APIKEY_PREFIX "ApiKey "

typedef struct {
    jwt_config_t config;
    airy_mtx_t lock;
    int initialized;
    char subject_buf[MAX_SUBJECT_SIZE];
    char role_buf[MAX_ROLE_SIZE];
} jwt_global_state_t;

extern jwt_global_state_t g_jwt;

typedef struct {
    apikey_config_t config;
    char **keys;
    size_t capacity;
    airy_mtx_t lock;
    int initialized;
    char subject_buf[512];
} apikey_global_state_t;

extern apikey_global_state_t g_apikey;

/**
 * @brief Client rate-limit entry
 */
typedef struct rate_limit_entry {
    char client_id[128];
    double tokens;
    double max_tokens;
    double refill_rate;
    time_t last_update;
    bool active;
} rate_limit_entry_t;

typedef struct {
    rate_limit_config_t config;
    rate_limit_entry_t entries[MAX_CLIENTS];
    airy_mtx_t lock;
    int initialized;
} ratelimit_global_state_t;

extern ratelimit_global_state_t g_ratelimit;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_SVC_AUTH_INTERNAL_H */
