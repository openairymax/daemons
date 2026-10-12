// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_security_audit.c
 * @brief daemon 安全域 - 审计事件域。
 *
 * 单元（五十三）归位：原 daemon_security_vault.c 中凭据存取与状态查询
 * 为零消费死面（存取语义已由产品壳 vault provider 直连承载，见
 * products/cupolas/daemon/src/security_dome.c），随该单元消除；唯一
 * 活件 daemon_audit_log_event 迁入本文件，文件名与职责一致。
 *
 * 审计是安全追踪证据（E-6）：写后 fsync 落盘保证断电不丢事件（P1-4）；
 * SVC_LOG 移到解锁后调用，消除 security→logger 双锁序耦合（P2-4）。
 */

#include "daemon_security_internal.h"

#include "error.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

int daemon_audit_log_event(const char *service_name, const char *operation, const char *resource,
                           int result, const char *agent_id)
{
    if (!service_name || !operation) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_err_t ctx_rc = sec_ctx_lock(__func__, AIRY_ERR_STATE_ERROR);
    if (ctx_rc != AIRY_OK) {
        return ctx_rc;
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
