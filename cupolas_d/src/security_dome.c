// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file security_dome.c
 * @brief 安全穹顶策略面：cupolas 穹顶统一引导 + vault provider 实现。
 *
 * 本单元是「机制/策略分离」在 daemons 层内策略侧的唯一落点：
 * svc_common 的安全机制核只认识 daemon_vault_provider_t 抽象表，
 * 穹顶身份（vault id）、口令来源（环境变量）与 cupolas 后端调用
 * 全部由本单元持有，经 daemon_security_config_t.vault_provider 注入。
 */

#include "daemon_security_dome.h"

#include "platform.h"
#include "cupolas.h"
#include "cupolas_entitlements.h"
#include "cupolas_error.h"
#include "cupolas_network_security.h"
#include "cupolas_vault.h"
#include "daemon_security.h"
#include "svc_logger.h"

#include <stdlib.h>

/* Vault policy: identity of the runtime keystore and where its passphrase
 * comes from. Production deployments must set the passphrase env var; an
 * unset value falls back to an empty passphrase (obfuscation only). */
#define DOME_VAULT_ID "agentrt"
#define DOME_VAULT_PASSWORD_ENV "AIRY_VAULT_PASSWORD"

static int g_dome_initialized = 0;
static int g_dome_pep_mode = 0;
static cupolas_vault_t *g_dome_vault = NULL;

/* ---------- cupolas vault provider (policy backend) ---------- */

static airy_err_t dome_vault_open(void **out_handle)
{
    if (!out_handle) {
        return AIRY_ERR_INVALID_PARAM;
    }

    const char *vault_password = getenv(DOME_VAULT_PASSWORD_ENV);
    if (!vault_password) {
        vault_password = "";
        SVC_LOG_WARN("dome_vault_open: %s not set - vault uses empty passphrase, "
                     "set it in production",
                     DOME_VAULT_PASSWORD_ENV);
    }

    cupolas_vault_t *vault = NULL;
    int rc = cupolas_vault_open(DOME_VAULT_ID, vault_password, &vault);
    if (rc != 0 || !vault) {
        SVC_LOG_ERROR("dome_vault_open: cupolas vault open FAILED (vault_id=%s, rc=%d)",
                      DOME_VAULT_ID, rc);
        return AIRY_ERR_UNKNOWN;
    }

    g_dome_vault = vault;
    *out_handle = vault;
    SVC_LOG_INFO("dome_vault_open: cupolas vault opened (vault_id=%s)", DOME_VAULT_ID);
    return AIRY_OK;
}

static void dome_vault_close(void *handle)
{
    if (!handle) {
        return;
    }
    cupolas_vault_close((cupolas_vault_t *)handle);
    if (handle == (void *)g_dome_vault) {
        g_dome_vault = NULL;
    }
}

static airy_err_t dome_vault_store(void *handle, const char *cred_id,
                                   cupolas_vault_cred_type_t cred_type, const uint8_t *data,
                                   size_t data_len, const char *owner)
{
    cupolas_vault_t *vault = (cupolas_vault_t *)handle;
    if (!vault || !cred_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    int rc = cupolas_vault_store(vault, cred_id, cred_type, data, data_len, NULL);
    if (rc != 0) {
        SVC_LOG_ERROR("dome_vault_store: cupolas_vault_store FAILED for cred_id=%s (rc=%d)",
                      cred_id, rc);
        return AIRY_ERR_UNKNOWN;
    }

    /* The storer is the default authorizer (read/write/delete); other agents
     * must be granted access explicitly (vault denies by default). A grant
     * failure keeps the credential stored and is only reported as a warning,
     * mirroring the historical daemon_store_credential semantics. */
    const char *cred_owner = owner ? owner : "system";
    int acl_rc = cupolas_vault_grant_access(vault, cred_id, cred_owner,
                                            CUPOLAS_VAULT_OP_READ | CUPOLAS_VAULT_OP_WRITE |
                                                CUPOLAS_VAULT_OP_DELETE,
                                            0);
    if (acl_rc != 0) {
        SVC_LOG_WARN("dome_vault_store: grant_access FAILED for cred_id=%s owner=%s (rc=%d)",
                     cred_id, cred_owner, acl_rc);
    }

    return AIRY_OK;
}

static airy_err_t dome_vault_retrieve(void *handle, const char *cred_id, const char *requester,
                                      uint8_t *data, size_t *data_len)
{
    cupolas_vault_t *vault = (cupolas_vault_t *)handle;
    if (!vault || !cred_id || !data || !data_len) {
        return AIRY_ERR_INVALID_PARAM;
    }

    int rc = cupolas_vault_retrieve(vault, cred_id, requester, data, data_len);
    if (rc == 0) {
        return AIRY_OK;
    }

    /* Backend codes are normalized here so the mechanism core never sees a
     * cupolas-specific error: a missing/void handle maps to NOT_FOUND, any
     * other failure is treated as an access denial (fail-closed). */
    SVC_LOG_DEBUG("dome_vault_retrieve: cred_id=%s agent=%s rc=%d (%s)", cred_id, requester, rc,
                  (rc == (int)cupolas_ERR_INVALID_PARAM || rc == (int)cupolas_ERR_NULL_POINTER)
                      ? "credential not found or access denied"
                      : "internal error");
    return (rc == (int)cupolas_ERR_NULL_POINTER) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_PERMISSION_DENIED;
}

static const daemon_vault_provider_t g_dome_vault_provider = {
    .open = dome_vault_open,
    .close = dome_vault_close,
    .store = dome_vault_store,
    .retrieve = dome_vault_retrieve,
};

cupolas_vault_t *cupolas_dome_vault(void)
{
    return g_dome_vault;
}

/* ---------- dome bootstrap (PDP / PEP wiring) ---------- */

/* 共享引导：daemon_security + cupolas 四层（sanitizer/workbench/audit）。
 * pep_mode=1 时跳过 vault/entitlements/net_security（PDP 集中持有，
 * PEP 经 RPC 转发访问——0.1.9 M2 §3.2）。 */
static airy_err_t dome_bootstrap(const char *daemon_name, int pep_mode)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("dome_bootstrap: NULL daemon_name");
        return AIRY_EINVAL;
    }

    if (g_dome_initialized) {
        SVC_LOG_DEBUG("dome_bootstrap: dome already initialized (daemon=%s)", daemon_name);
        return AIRY_SUCCESS;
    }

    /* AIRY_HOME path layout: every daemon ensures its dirs exist at startup
     * (idempotent). Audit logs, sockets and agent child logs live under it. */
    airy_paths_init();

    /* P3.15 ACC-DT16: explicitly initialize the daemon_security layer
     * (non-NULL config). The dome owns the policy: the cupolas vault provider
     * is injected here and the mechanism core holds no backend dependency. */
    daemon_security_config_t sec_config;
    __builtin_memset(&sec_config, 0, sizeof(sec_config));
    sec_config.sanitize_level = SANITIZE_LEVEL_HIGH;
    sec_config.sanitizer_rules_path = NULL;
    /* Tool-level permission rules (SSoT): $AIRY_CONFIG_DIR/cupolas/permission_rules.yaml.
     * Absent file keeps the ACL empty (fail-closed) so tool_d denies all tools.
     * 兼容旧部署：cupolas/ 子目录缺失时回退到 $AIRY_CONFIG_DIR/permission_rules.yaml，
     * 避免规则文件存在却因目录约定不一致导致 ACL 为空（subagent 工具全部被拒）。 */
    static char g_perm_rules_path[512];
    snprintf(g_perm_rules_path, sizeof(g_perm_rules_path), "%s/cupolas/permission_rules.yaml",
             airy_config_dir());
    if (!airy_file_exists(g_perm_rules_path)) {
        snprintf(g_perm_rules_path, sizeof(g_perm_rules_path), "%s/permission_rules.yaml",
                 airy_config_dir());
    }
    sec_config.permission_rules_path = g_perm_rules_path;
    sec_config.enable_permission_cache = true;
    sec_config.enable_signature_verification = false;
    sec_config.trusted_ca_path = NULL;
    sec_config.expected_signer = NULL;
    sec_config.enable_vault = true;
    sec_config.vault_provider = &g_dome_vault_provider;
    sec_config.enable_audit_logging = true;
    sec_config.audit_log_dir = NULL;

    airy_err_t sec_err = AIRY_OK;
    int sec_rc = daemon_security_init(&sec_config, &sec_err);
    if (sec_rc != 0) {
        SVC_LOG_ERROR("dome_bootstrap: daemon_security_init FAILED for daemon='%s' "
                      "(rc=%d, err=%d) — security layer unavailable, "
                      "service-layer fail-closed will deny all privileged operations",
                      daemon_name, sec_rc, (int)sec_err);
    }

    airy_err_t cupolas_err = AIRY_OK;
    /* M2-S5（0.1.9 §3.2）：pep 最小 guard 不构造本地 permission 引擎
     * （策略由 PDP cupolas_d 唯一持有），sanitizer/workbench/audit 保留。 */
    int rc = pep_mode ? cupolas_init_pep(NULL, &cupolas_err) : cupolas_init(NULL, &cupolas_err);
    if (rc != 0) {
        SVC_LOG_ERROR("dome_bootstrap: cupolas_init FAILED for daemon='%s' "
                      "(rc=%d, err=%d) — security dome unavailable, "
                      "service-layer fail-closed will deny all privileged operations",
                      daemon_name, rc, (int)cupolas_err);
        return cupolas_err;
    }

    if (pep_mode) {
        /* PEP 最小 guard：本地 vault/entitlements/netsec 不初始化，
         * 由 PDP（cupolas_d）集中持有，PEP 经 RPC 转发访问。 */
        g_dome_pep_mode = 1;
        g_dome_initialized = 1;
        SVC_LOG_INFO("dome_bootstrap: cupolas PEP minimal-guard initialized for '%s' "
                     "(sanitizer + workbench + audit_logger + daemon_security; "
                     "vault/entitlements/netsec via PDP RPC)",
                     daemon_name);
        return AIRY_SUCCESS;
    }

    /* Wiring: vault / entitlements / network security submodules hooked
     * into the daemon runtime chain. Failure of any of the three inits is
     * non-fatal: the service layer's fail-closed logic blocks privileged
     * operations. */
    int vault_rc = cupolas_vault_init(NULL);
    if (vault_rc != 0) {
        SVC_LOG_ERROR("dome_bootstrap: cupolas_vault_init FAILED for daemon='%s' (rc=%d)",
                      daemon_name, vault_rc);
    }

    int entitlements_rc = cupolas_entitlements_init();
    if (entitlements_rc != 0) {
        SVC_LOG_ERROR(
            "dome_bootstrap: cupolas_entitlements_init FAILED for daemon='%s' (rc=%d)",
            daemon_name, entitlements_rc);
    }

    int net_rc = cupolas_net_security_init(NULL);
    if (net_rc != 0) {
        SVC_LOG_ERROR("dome_bootstrap: cupolas_net_security_init FAILED for daemon='%s' (rc=%d)",
                      daemon_name, net_rc);
    }

    g_dome_initialized = 1;
    SVC_LOG_INFO("dome_bootstrap: cupolas security dome initialized for '%s' "
                 "(permission_engine + sanitizer + workbench + audit_logger + daemon_security "
                 "+ vault + entitlements + network_security)",
                 daemon_name);
    return AIRY_SUCCESS;
}

airy_err_t daemon_dome_init(const char *daemon_name)
{
    return dome_bootstrap(daemon_name, 0);
}

airy_err_t daemon_dome_init_pep(const char *daemon_name)
{
    return dome_bootstrap(daemon_name, 1);
}

void daemon_dome_cleanup(void)
{
    if (!g_dome_initialized) {
        return;
    }

    if (!g_dome_pep_mode) {
        cupolas_vault_cleanup();
        cupolas_entitlements_cleanup();
        cupolas_net_security_cleanup();
    }

    cupolas_flush_audit_log();
    cupolas_cleanup();
    daemon_security_shutdown();
    g_dome_initialized = 0;
    g_dome_pep_mode = 0;
    SVC_LOG_INFO("daemon_dome_cleanup: cupolas security dome shut down");
}
