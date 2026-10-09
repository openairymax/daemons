// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_security_vault.c
 * @brief Daemon Layer Security - vault credentials, audit and status domain.
 *
 * Phase 2.3a split from daemon_security.c: credential storage/retrieval
 * delegated to an injected vault provider (ACL fail-closed), the durable
 * audit log (E-6 traceability, fsync after every event), and module status
 * introspection.
 *
 * The public API surface (daemon_security.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/daemon_security.c (init/sanitize domain)
 * @see agentrt/daemons/common/src/daemon_security_internal.h
 */

#include "daemon_security_internal.h"

#include "error.h"

#include "cupolas_vault_cred_type.h"

#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

int daemon_store_credential(const char *cred_id, cupolas_vault_cred_type_t cred_type,
                            const uint8_t *data, size_t data_len, const char *agent_id)
{
    if (!cred_id || !data || data_len == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_store_credential: daemon_security not initialized — "
                      "initialize at startup. DENYING credential storage (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.vault_enabled || !g_security_ctx.vault ||
        !g_security_ctx.vault_provider || !g_security_ctx.vault_provider->store) {
        airy_mtx_unlock(&g_security_mutex);
        AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "vault is disabled or unavailable");
    }

    /* The storer is the default authorizer (read/write/delete); the provider
     * grants it access on store (vault denies by default, fail-closed). */
    const char *owner = agent_id ? agent_id : "system";
    airy_err_t rc =
        g_security_ctx.vault_provider->store(g_security_ctx.vault, cred_id, cred_type, data,
                                             data_len, owner);
    if (rc != AIRY_OK) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_store_credential: vault store FAILED for cred_id=%s (rc=%d)", cred_id,
                      (int)rc);
        return AIRY_ERR_UNKNOWN;
    }

    airy_mtx_unlock(&g_security_mutex);
    SVC_LOG_INFO("Credential stored in vault: %s (type=%d, %zu bytes, owner=%s)", cred_id,
                 cred_type, data_len, owner);
    return AIRY_OK;
}

int daemon_retrieve_credential(const char *cred_id, const char *agent_id, uint8_t *data,
                               size_t *data_len)
{
    if (!cred_id || !data || !data_len) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_retrieve_credential: daemon_security not initialized — "
                      "initialize at startup. DENYING credential retrieval (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.vault_enabled || !g_security_ctx.vault ||
        !g_security_ctx.vault_provider || !g_security_ctx.vault_provider->retrieve) {
        airy_mtx_unlock(&g_security_mutex);
        AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "vault is disabled or unavailable");
    }

    /* Keep the "system" exemption semantics: system-level calls carry no
     * agent constraint and are left to the vault's ACL; all other requests
     * are scoped by agent_id (fail-closed). */
    const char *requester = agent_id ? agent_id : "system";

    size_t buf_len = *data_len;
    airy_err_t rc = g_security_ctx.vault_provider->retrieve(g_security_ctx.vault, cred_id,
                                                            requester, data, &buf_len);
    if (rc != AIRY_OK) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("daemon_retrieve_credential: vault retrieve FAILED for cred_id=%s "
                     "agent=%s (rc=%d)",
                     cred_id, requester, (int)rc);
        return rc;
    }

    *data_len = buf_len;
    airy_mtx_unlock(&g_security_mutex);
    return AIRY_OK;
}

int daemon_audit_log_event(const char *service_name, const char *operation, const char *resource,
                           int result, const char *agent_id)
{
    if (!service_name || !operation) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    /* P2-3：状态检查移入锁内，避免与 shutdown 的时序竞态 */
    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_audit_log_event: daemon_security not initialized — "
                      "initialize at startup. Audit event DROPPED (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }
    if (!g_security_ctx.audit_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        return 0;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &tm_info);

    const char *result_str = (result == 0) ? "SUCCESS" : "FAILED";
    char log_msg[MAX_AUDIT_LOG_SIZE];

    snprintf(log_msg, sizeof(log_msg),
             "[%s] [%s] service=%s operation=%s resource=%s agent=%s result=%s\n", timestamp,
             result_str, service_name, operation, resource ? resource : "N/A",
             agent_id ? agent_id : "system", result_str);

    if (g_security_ctx.audit_fp) {
        /* P1-4：审计是安全追踪证据（E-6），fflush 只刷到内核页缓存，
         * 断电/崩溃会丢事件；写后 fsync 保证落盘。 */
        if (fwrite(log_msg, 1, strlen(log_msg), g_security_ctx.audit_fp) == strlen(log_msg) &&
            fflush(g_security_ctx.audit_fp) == 0) {
#ifdef _WIN32
            _commit(_fileno(g_security_ctx.audit_fp));
#else
            fsync(fileno(g_security_ctx.audit_fp));
#endif
        }
    } else {
        /* P2-4：SVC_LOG 内部取 g_log_file_mutex，移到解锁后再调用，
         * 消除 security→logger 双锁序耦合（锁内只做文件 I/O）。 */
        airy_mtx_unlock(&g_security_mutex);
        if (result == 0) {
            SVC_LOG_INFO("[AUDIT] %s", log_msg);
        } else {
            SVC_LOG_WARN("[AUDIT] %s", log_msg);
        }
        return AIRY_OK;
    }
    airy_mtx_unlock(&g_security_mutex);

    return AIRY_OK;
}

int daemon_security_get_status(int *sanitizer_status, int *permission_status, int *signature_status,
                               int *vault_status, int *audit_status)
{
    if (!sanitizer_status || !permission_status || !signature_status || !vault_status ||
        !audit_status) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    *sanitizer_status = g_security_ctx.initialized ? 1 : 0;
    *permission_status = g_security_ctx.permission_enabled ? 1 : 0;
    *signature_status = g_security_ctx.signature_enabled ? 1 : 0;
    *vault_status = g_security_ctx.vault_enabled ? 1 : 0;
    *audit_status = g_security_ctx.audit_enabled ? 1 : 0;
    airy_mtx_unlock(&g_security_mutex);

    return AIRY_OK;
}
