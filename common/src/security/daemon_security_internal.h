/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_security_internal.h
 * @brief Internal shared definitions of the daemon_security sources
 *        (not public API).
 *
 * After the Phase 2.3a split of daemon_security.c into four sources, this
 * header carries the shared contract between the pieces:
 *   - daemon_security.c            init/shutdown + input sanitization
 *   - daemon_security_acl.c        ACL permission checks + rule management
 *   - daemon_security_signature.c  ED25519 package signature verification
 *   - daemon_security_vault.c      vault credentials + audit + status
 *
 * Shared state (g_security_ctx / g_security_mutex) is defined in
 * daemon_security.c; all domains serialize on g_security_mutex via
 * ensure_mutex_initialized() (three-state lazy init, see definition).
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
#include "cupolas_vault.h"
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

typedef struct {
    char agent_id[64];
    char resource[128];
    uint32_t operations;
    bool allowed;
} acl_entry_t;

/**
 * @brief Daemon security module global state (defined in daemon_security.c)
 */
typedef struct {
    bool initialized;
    sanitize_level_t current_sanitize_level;
    bool permission_enabled;
    bool signature_enabled;
    bool vault_enabled;
    bool audit_enabled;
    cupolas_vault_t *vault;
    acl_entry_t acl_table[MAX_ACL_ENTRIES];
    size_t acl_count;
    FILE *audit_fp;
    char audit_log_path[256];
} daemon_security_ctx_t;

extern daemon_security_ctx_t g_security_ctx;
extern airy_mtx_t g_security_mutex; /* CROSS-01: initialized via airy_mtx_init() */

/**
 * @brief Three-state lazy init of g_security_mutex (0=uninit, 2=initing,
 *        1=ready). Aligns with the hall_writer/gateway_hall_store CAS
 *        paradigm; prevents concurrent duplicate pthread_mutex_init (UB).
 */
void ensure_mutex_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_DAEMON_SECURITY_INTERNAL_H */
