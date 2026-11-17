/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_security_internal.h
 * @brief Internal shared definitions of the daemon_security sources
 *        (not public API).
 *
 * After the Phase 2.3a split of daemon_security.c into three sources, this
 * header carries the shared contract between the pieces:
 *   - daemon_security.c            init/shutdown + input sanitization
 *   - daemon_security_acl.c        ACL permission checks + rule management
 *   - daemon_security_audit.c      audit event logging
 *   - daemon_security_rotate.c     0.1.21 A-3: audit log rotation (size cap)
 *
 * Shared state (g_security_ctx / g_security_mutex) is defined in
 * daemon_security.c; all domains serialize on g_security_mutex via
 * sec_ctx_lock() (mutex lazy init lives inside daemon_security.c).
 *
 * This header is for the daemon_security sources only; it must not be used
 * by other modules.
 *
 * @see agentrt/daemons/common/include/daemon_security.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_DAEMON_SECURITY_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_DAEMON_SECURITY_INTERNAL_H

#include "daemon_security.h"

#include "atomic_compat.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SVC_LOG_SECURITY
#define SVC_LOG_SECURITY(...) AIRY_LOG_WARN(__VA_ARGS__)
#endif

/* ACL 表容量：每个 agent 注册 12 个 builtin 工具 + 各服务主体（tool_d、
 * external 等）。128 曾因 AIRY_AGENT_ACL 注入 11 个 agent × 12 工具
 * （132 条）而溢出，导致 tool_d 主体规则添加失败、全部工具 fail-closed
 * 拒绝。256 覆盖 16 个 agent × 12 工具 + 保留余量。 */
#define MAX_ACL_ENTRIES 256
#define MAX_AUDIT_LOG_SIZE 1024

/* 审计轮转契约（docs-linux/50-engineering-standards/20-contracts/contracts.md）：
 * 500MB/天、保留 365 份、zstd 压缩 + AES-256-GCM 加密。契约值只在本处
 * 声明一次，实现与契约测试都从这里取值，避免两处漂移。 */
#define AUD_MAX_BYTES_DEF (500ULL * 1024ULL * 1024ULL)
#define AUD_KEEP_FILES_DEF 365U

typedef struct {
    char agent_id[64];
    char resource[128];
    uint32_t operations;
    bool allowed;
} acl_entry_t;

/** 审计轮转生效策略：env 可在排障时覆盖（AIRY_AUDIT_MAX_BYTES=0 关闭）。 */
typedef struct {
    unsigned long long max_bytes; /**< 单文件上限；0 = 关闭轮转 */
    unsigned int keep_files;      /**< 备份保留份数 */
    unsigned int compress;        /**< 1 = 构建期具备 zstd */
    unsigned int encrypt;         /**< 1 = 构建期具备 OpenSSL 且密钥可用 */
} audit_rot_policy_t;

/**
 * @brief Daemon security module global state (defined in daemon_security.c)
 */
typedef struct {
    bool initialized;
    sanitize_level_t current_sanitize_level;
    bool permission_enabled;
    bool vault_enabled;
    bool audit_enabled;
    void *vault; /**< Opaque handle owned by vault_provider */
    const daemon_vault_provider_t *vault_provider; /**< Injected policy backend */
    acl_entry_t acl_table[MAX_ACL_ENTRIES];
    size_t acl_count;
    FILE *audit_fp;
    char audit_log_path[256];
    unsigned long long audit_bytes; /**< 当前文件字节数（轮转判据） */
    audit_rot_policy_t audit_rot;   /**< 初始化时解析的轮转策略 */
    char audit_key_path[256];       /**< 加密密钥文件（空 = 不加密） */
} daemon_security_ctx_t;

extern daemon_security_ctx_t g_security_ctx;
extern airy_mtx_t g_security_mutex; /* CROSS-01: initialized via airy_mtx_init() */

/**
 * @brief Acquire g_security_mutex under the fail-closed context guard.
 *
 * Collapses the "lock, reject if uninitialized" prologue duplicated across
 * the security domains into one mechanism. On success the
 * mutex is held and the caller must unlock it; when the module is not
 * initialized the mutex is left unlocked, a fail-closed error is logged
 * with the caller name, and @p deny is returned.
 *
 * @param who  Caller name for the denial log (pass __func__)
 * @param deny Error code to return when the module is uninitialized
 * @return AIRY_OK with the mutex held, or @p deny with the mutex unlocked
 */
airy_err_t sec_ctx_lock(const char *who, airy_err_t deny);

/**
 * @brief Load tool-level permission rules from a YAML file into the ACL
 *        table (internal: invoked by daemon_security_init(), no external
 *        consumers).
 *
 * Rule schema (tool-level, matches daemon_check_tool_permission()'s
 * (agent, tool) lookup):
 *
 * @code
 * rules:
 *   - agent: "coding_v1"
 *     tool: "fs_read"
 *     effect: "allow"
 *   - agent: "coding_v1"
 *     tool: "shell_run"
 *     effect: "deny"
 * @endcode
 *
 * Only rules with a non-empty agent, tool and effect ("allow"/"deny") are
 * applied; malformed entries are skipped. Missing files return
 * AIRY_ERR_NOT_FOUND, unreadable files AIRY_ERR_PARSE_ERROR. The table
 * stays fail-closed: tools without an allow rule are denied.
 *
 * @param path Absolute path of the permission rules YAML file
 * @return AIRY_OK on success (0 rules loaded counts as success when the
 *         file parsed), error code otherwise
 */
int daemon_security_load_rules_file(const char *path);

/* ---- daemon_security_rotate.c：审计轮转（机制，无策略落点） ----
 * 调用约定：三个函数都要求调用方已持有 g_security_mutex（audit 写入路径
 * 在锁内调用 aud_rot_apply）。aud_rot_init 在 audit_fp 打开后调用。 */
void aud_rot_policy(audit_rot_policy_t *out);
int aud_rot_init(void);
int aud_rot_apply(unsigned long long pending);
/** 取证还原：备份(.zst/.enc) → 明文输出文件；供运维与契约用例使用。 */
int aud_rot_restore(const char *backup, const char *out_path);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_DAEMON_SECURITY_INTERNAL_H */
