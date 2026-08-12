// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_cupolas_bootstrap.c
 * @brief P3.14 (ACC-DT15): unified cupolas security dome bootstrap impl.
 */

#include "daemon_cupolas_bootstrap.h"

#include "platform.h"
#include "cupolas.h"
#include "cupolas_entitlements.h"
#include "cupolas_network_security.h"
#include "cupolas_vault.h"
#include "daemon_security.h"
#include "svc_logger.h"

static int g_cupolas_initialized = 0;

airy_err_t daemon_cupolas_init(const char *daemon_name)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("daemon_cupolas_init: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_cupolas_initialized) {
        SVC_LOG_DEBUG("daemon_cupolas_init: cupolas already initialized (daemon=%s)", daemon_name);
        return AIRY_SUCCESS;
    }

    /* AIRY_HOME path layout: every daemon ensures its dirs exist at startup
     * (idempotent). Audit logs, sockets and agent child logs live under it. */
    airy_paths_init();

    /* P3.15 ACC-DT16: explicitly initialize the daemon_security layer
     * (non-NULL config). Legacy code lazy-initialized inside each security
     * function (daemon_security_init(NULL,NULL)), hiding the fact that a
     * daemon never explicitly initialized the security layer. Now the
     * cupolas bootstrap does one explicit init; security functions are
     * fail-closed (uninitialized = denied). */
    daemon_security_config_t sec_config;
    __builtin_memset(&sec_config, 0, sizeof(sec_config));
    sec_config.sanitize_level = SANITIZE_LEVEL_STRICT;
    sec_config.sanitizer_rules_path = NULL;
    sec_config.permission_rules_path = NULL;
    sec_config.enable_permission_cache = true;
    sec_config.enable_signature_verification = false;
    sec_config.trusted_ca_path = NULL;
    sec_config.expected_signer = NULL;
    sec_config.enable_vault = true;
    sec_config.vault_storage_path = NULL;
    sec_config.enable_audit_logging = true;
    sec_config.audit_log_dir = NULL;

    airy_err_t sec_err = AIRY_OK;
    int sec_rc = daemon_security_init(&sec_config, &sec_err);
    if (sec_rc != 0) {
        SVC_LOG_ERROR("daemon_cupolas_init: daemon_security_init FAILED for daemon='%s' "
                      "(rc=%d, err=%d) — security layer unavailable, "
                      "service-layer fail-closed will deny all privileged operations",
                      daemon_name, sec_rc, (int)sec_err);
    }

    airy_err_t cupolas_err = AIRY_OK;
    int rc = cupolas_init(NULL, &cupolas_err);
    if (rc != 0) {
        SVC_LOG_ERROR("daemon_cupolas_init: cupolas_init FAILED for daemon='%s' "
                      "(rc=%d, err=%d) — security dome unavailable, "
                      "service-layer fail-closed will deny all privileged operations",
                      daemon_name, rc, (int)cupolas_err);
        return cupolas_err;
    }

    /* Wiring: vault / entitlements / network security submodules hooked
     * into the daemon runtime chain. Previously only the four layers
     * permission_engine + sanitizer + workbench + audit were wired;
     * vault/entitlements/network had zero callers (test-only references),
     * so those security capabilities never took effect. Failure of any of
     * the three inits is non-fatal: the service layer's fail-closed logic
     * blocks privileged operations. */
    int vault_rc = cupolas_vault_init(NULL);
    if (vault_rc != 0) {
        SVC_LOG_ERROR("daemon_cupolas_init: cupolas_vault_init FAILED for daemon='%s' (rc=%d)",
                      daemon_name, vault_rc);
    }

    int entitlements_rc = cupolas_entitlements_init();
    if (entitlements_rc != 0) {
        SVC_LOG_ERROR(
            "daemon_cupolas_init: cupolas_entitlements_init FAILED for daemon='%s' (rc=%d)",
            daemon_name, entitlements_rc);
    }

    int net_rc = cupolas_net_security_init(NULL);
    if (net_rc != 0) {
        SVC_LOG_ERROR(
            "daemon_cupolas_init: cupolas_net_security_init FAILED for daemon='%s' (rc=%d)",
            daemon_name, net_rc);
    }

    g_cupolas_initialized = 1;
    SVC_LOG_INFO("daemon_cupolas_init: cupolas security dome initialized for '%s' "
                 "(permission_engine + sanitizer + workbench + audit_logger + daemon_security "
                 "+ vault + entitlements + network_security)",
                 daemon_name);
    return AIRY_SUCCESS;
}

void daemon_cupolas_cleanup(void)
{
    if (!g_cupolas_initialized)
        return;

    cupolas_vault_cleanup();
    cupolas_entitlements_cleanup();
    cupolas_net_security_cleanup();

    cupolas_flush_audit_log();
    cupolas_cleanup();
    daemon_security_shutdown();
    g_cupolas_initialized = 0;
    SVC_LOG_INFO("daemon_cupolas_cleanup: cupolas security dome shut down");
}
