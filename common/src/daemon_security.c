// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
#include "platform.h"
/*
 *
 * daemon_security.c - Daemon Layer Security Integration Implementation
 */

/**
 * @file daemon_security.c
 * @brief Daemon Layer Security Integration - Unified Security for All Daemon Services
 * @author SPHARX Ltd. - Airymax Team
 * @date 2026-04-02
 *
 * This module provides unified security integration for all AgentRT daemon services:
 * - tool_d: Tool execution security (sanitization + permission)
 * - llm_d: LLM service security (input sanitization + API key protection)
 * - market_d: Market service security (package signature verification)
 *
 * Design Principles:
 * - E-1 Security by Default: All services must use these functions
 * - K-4 Zero Trust Authorization: Every request validated
 * - E-6 Error Traceability: Complete error handling with audit trail
 */

#include "daemon_security.h"

#include "yaml_minimal.h"

#ifndef SVC_LOG_SECURITY
#define SVC_LOG_SECURITY(...) AIRY_LOG_WARN(__VA_ARGS__)
#endif

#include "daemon_platform_ext.h"
#include "svc_logger.h"
#include "atomic_compat.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* pthread.h provided by platform.h — no direct pthread include (CROSS-01) */
/* Internal state structure */
static struct {
    bool initialized;
    sanitize_level_t current_sanitize_level;
    bool permission_enabled;
    bool signature_enabled;
    bool vault_enabled;
    bool audit_enabled;
} g_daemons_security
    __attribute__((unused)) = {false, SANITIZE_LEVEL_NORMAL, false, false, false, false};

/* ---------- Initialization and Shutdown ---------- */
#include "airy_dirent.h"
#include "cupolas_error.h"
#include "cupolas_signer_info.h"
#include "cupolas_vault.h"
#include "cupolas_vault_cred_type.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef AIRY_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

/* ACL 表容量：每个 agent 注册 12 个 builtin 工具 + 各服务主体（tool_d、
 * external 等）。128 曾因 AIRY_AGENT_ACL 注入 11 个 agent × 12 工具
 * （132 条）而溢出，导致 tool_d 主体规则添加失败、全部工具 fail-closed
 * 拒绝。256 覆盖 16 个 agent × 12 工具 + 保留余量。 */
#define MAX_ACL_ENTRIES 256
#define MAX_AUDIT_LOG_SIZE 1024
#define DAEMON_VAULT_ID "agentrt"
#define DAEMON_VAULT_PASSWORD_ENV "AIRY_VAULT_PASSWORD"

typedef struct {
    char agent_id[64];
    char resource[128];
    uint32_t operations;
    bool allowed;
} acl_entry_t;

static struct {
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
} g_security_ctx = {0};

static airy_mtx_t g_security_mutex; /* CROSS-01: initialized via airy_mtx_init() */
/* 三态惰性初始化（P1-3）：0=未初始化，2=初始化中，1=就绪。
 * 对齐 hall_writer/gateway_hall_store 的 CAS 范式，杜绝并发重复
 * pthread_mutex_init（UB）。 */
static atomic_int g_security_mutex_ready = 0;

static void ensure_mutex_initialized(void)
{
    while (atomic_load_explicit(&g_security_mutex_ready, memory_order_acquire) != 1) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&g_security_mutex_ready, &expected, 2,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            airy_mtx_init(&g_security_mutex);
            atomic_store_explicit(&g_security_mutex_ready, 1, memory_order_release);
            break;
        }
    }
}

static const char *DANGEROUS_PATTERNS[] = {";",  "|", "`",  "$(", "${", "&&", "||", ">",
                                           ">>", "<", "<<", "\\", "\n", "\r", NULL};

static bool contains_dangerous_pattern(const char *input)
{
    if (!input)
        return false;
    for (size_t i = 0; DANGEROUS_PATTERNS[i] != NULL; i++) {
        if (strstr(input, DANGEROUS_PATTERNS[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void sanitize_string(char *output, const char *input, size_t max_len)
{
    if (!output || !input || max_len == 0)
        return;

    size_t j = 0;
    for (size_t i = 0; input[i] && j < max_len - 1; i++) {
        unsigned char c = (unsigned char)input[i];
        if (isprint(c) && c != '\\' && c != '\n' && c != '\r' && c != '\t') {
            output[j++] = (char)c;
        }
    }
    output[j] = '\0';
}

int daemon_security_init(const daemon_security_config_t *config, airy_err_t *error)
{
    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    if (g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_INFO("Daemon security: already initialized");
        return 0;
    }

    __builtin_memset(&g_security_ctx, 0, sizeof(g_security_ctx));
    g_security_ctx.current_sanitize_level = SANITIZE_LEVEL_STRICT;
    g_security_ctx.permission_enabled = true;
    g_security_ctx.signature_enabled = true;
    g_security_ctx.vault_enabled = true;
    g_security_ctx.audit_enabled = true;

    if (config) {
        g_security_ctx.current_sanitize_level = config->sanitize_level;
        g_security_ctx.permission_enabled = config->enable_permission_cache;
        g_security_ctx.signature_enabled = config->enable_signature_verification;
        g_security_ctx.vault_enabled = config->enable_vault;
        g_security_ctx.audit_enabled = config->enable_audit_logging;

        if (config->audit_log_dir && strlen(config->audit_log_dir) > 0) {
            snprintf(g_security_ctx.audit_log_path, sizeof(g_security_ctx.audit_log_path),
                     "%s/daemon_audit.log", config->audit_log_dir);
        }
    }

    if (g_security_ctx.audit_enabled && g_security_ctx.audit_log_path[0] == '\0') {

        snprintf(g_security_ctx.audit_log_path, sizeof(g_security_ctx.audit_log_path),
                 "%s/daemon_audit.log", airy_log_dir());
    }

    if (g_security_ctx.audit_enabled) {
        g_security_ctx.audit_fp = fopen(g_security_ctx.audit_log_path, "a");
        if (!g_security_ctx.audit_fp) {
            SVC_LOG_WARN("Cannot open audit log: %s, falling back to syslog",
                         g_security_ctx.audit_log_path);
        }
    }

    /* Wire up cupolas_vault: credential storage is encrypted and kept by the
     * vault (no longer a self-maintained plaintext in-memory array). Passphrase
     * source: the AIRY_VAULT_PASSWORD env var; when unset, an empty passphrase
     * derives the master key and credentials are only obfuscated (not a
     * production security baseline) — production deployments must configure
     * this env var. */
    if (g_security_ctx.vault_enabled) {
        const char *vault_password = getenv(DAEMON_VAULT_PASSWORD_ENV);
        if (!vault_password) {
            vault_password = "";
            SVC_LOG_WARN("daemon_security: %s not set - vault uses empty passphrase, "
                         "set it in production",
                         DAEMON_VAULT_PASSWORD_ENV);
        }
        cupolas_vault_t *vault = NULL;
        int vault_rc = cupolas_vault_open(DAEMON_VAULT_ID, vault_password, &vault);
        if (vault_rc == 0 && vault) {
            g_security_ctx.vault = vault;
            SVC_LOG_INFO("daemon_security: cupolas vault opened (vault_id=%s)", DAEMON_VAULT_ID);
        } else {
            g_security_ctx.vault_enabled = false;
            SVC_LOG_ERROR("daemon_security: cupolas vault open FAILED (vault_id=%s, rc=%d) "
                          "— credential storage disabled (fail-closed)",
                          DAEMON_VAULT_ID, vault_rc);
        }
    }

    g_security_ctx.initialized = true;
    airy_mtx_unlock(&g_security_mutex);
    SVC_LOG_INFO("Daemon security: initialized in production mode (sanitize_level=%d)",
                 g_security_ctx.current_sanitize_level);

    /* Load tool-level permission rules (fail-closed: absent file means no
     * ACL entries, so every tool permission check denies). */
    if (config && config->permission_rules_path && config->permission_rules_path[0]) {
        int lrc = daemon_security_load_rules_file(config->permission_rules_path);
        if (lrc == AIRY_ERR_NOT_FOUND) {
            SVC_LOG_WARN("daemon_security: no permission rules file at %s — "
                         "tool permission checks will deny (fail-closed)",
                         config->permission_rules_path);
        } else if (lrc != AIRY_OK) {
            SVC_LOG_WARN("daemon_security: permission rules load skipped for %s (rc=%d)",
                         config->permission_rules_path, lrc);
        }
    }
    return 0;
}

void daemon_security_shutdown(void)
{
    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        return;
    }

    if (g_security_ctx.vault) {
        cupolas_vault_close(g_security_ctx.vault);
        g_security_ctx.vault = NULL;
    }

    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        g_security_ctx.acl_table[i].agent_id[0] = '\0';
        g_security_ctx.acl_table[i].resource[0] = '\0';
    }
    g_security_ctx.acl_count = 0;

    if (g_security_ctx.audit_fp) {
        fclose(g_security_ctx.audit_fp);
        g_security_ctx.audit_fp = NULL;
    }

    g_security_ctx.initialized = false;
    g_security_ctx.permission_enabled = false;
    g_security_ctx.signature_enabled = false;
    g_security_ctx.vault_enabled = false;
    g_security_ctx.audit_enabled = false;
    airy_mtx_unlock(&g_security_mutex);
    airy_mtx_destroy(&g_security_mutex);
    atomic_store_explicit(&g_security_mutex_ready, 0, memory_order_release);
    SVC_LOG_INFO("Daemon security: shutdown complete");
}

int daemon_sanitize_llm_input(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    /* P3.15 ACC-DT16: fail-safe — no more lazy-init. When uninitialized,
     * keep sanitizing with the strictest SANITIZE_LEVEL_STRICT (sanitizing is
     * a protective operation; refusing would reduce security). Callers should
     * initialize explicitly via daemon_cupolas_init() at daemon startup. */
    sanitize_level_t level;
    if (!g_security_ctx.initialized) {
        level = SANITIZE_LEVEL_STRICT;
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN(
            "daemon_sanitize_llm_input: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. Using SANITIZE_LEVEL_STRICT (fail-safe).");
    } else {
        level = g_security_ctx.current_sanitize_level;
        airy_mtx_unlock(&g_security_mutex);
    }

    if (contains_dangerous_pattern(input)) {
        SVC_LOG_SECURITY(
            "SEC-011 VIOLATION: LLM input contains shell injection pattern - REJECTED");
        snprintf(output, output_size, "[SANITIZED: input rejected - security violation]");
        return AIRY_ERR_PERMISSION_DENIED;
    }

    sanitize_string(output, input, output_size);

    if (level >= SANITIZE_LEVEL_STRICT) {
        for (size_t i = 0; output[i]; i++) {
            if ((unsigned char)output[i] > 127) {
                output[i] = '?';
            }
        }
    }

    return AIRY_OK;
}

int daemon_sanitize_tool_params(const char *tool_name, const char *params, char *sanitized_tool,
                                size_t tool_buf_size, char *sanitized_params, size_t param_buf_size)
{
    if (!tool_name || !params || !sanitized_tool || !sanitized_params) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    /* P3.15 ACC-DT16: fail-safe — no more lazy-init. When uninitialized,
     * still sanitize (sanitizing is a protective operation), only warn. */
    bool sec_uninitialized = !g_security_ctx.initialized;
    airy_mtx_unlock(&g_security_mutex);
    if (sec_uninitialized) {
        SVC_LOG_WARN("daemon_sanitize_tool_params: daemon_security not initialized — "
                     "call daemon_cupolas_init() during startup. Proceeding with default sanitize "
                     "(fail-safe).");
    }

    sanitize_string(sanitized_tool, tool_name, tool_buf_size);

    if (contains_dangerous_pattern(params)) {
        SVC_LOG_SECURITY("SEC-014 VIOLATION: Tool params contain dangerous pattern - REJECTED");
        snprintf(sanitized_params, param_buf_size, "[SANITIZED: params rejected]");
        return AIRY_ERR_PERMISSION_DENIED;
    }

    sanitize_string(sanitized_params, params, param_buf_size);

    if (strlen(params) > param_buf_size - 1) {
        SVC_LOG_WARN("Tool params truncated: %zu -> %zu bytes", strlen(params), param_buf_size - 1);
    }

    return AIRY_OK;
}

int daemon_check_tool_permission(const char *agent_id, const char *tool_name, const char *action)
{
    if (!agent_id || !tool_name || !action) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_check_tool_permission: daemon_security not initialized — "
                      "call daemon_cupolas_init() during startup. DENYING %s/%s (fail-closed).",
                      agent_id, tool_name);
        return AIRY_EPERM;
    }

    if (!g_security_ctx.permission_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("Permission check: disabled by configuration, DENYING %s/%s (fail-closed)",
                     agent_id, tool_name);
        return AIRY_EPERM;
    }

    int result = AIRY_ERR_PERMISSION_DENIED;
    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strcmp(g_security_ctx.acl_table[i].resource, tool_name) == 0) {

            if (g_security_ctx.acl_table[i].allowed) {
                result = AIRY_OK;
            }
            break;
        }
    }
    airy_mtx_unlock(&g_security_mutex);

    if (result == AIRY_OK) {
        SVC_LOG_DEBUG("Permission GRANTED: agent=%s tool=%s action=%s", agent_id, tool_name,
                      action);
    } else {
        SVC_LOG_SECURITY("Permission DENIED (no ACL entry): agent=%s tool=%s action=%s", agent_id,
                         tool_name, action);
    }
    return result;
}

int daemon_check_llm_permission(const char *agent_id, const char *model_name, const char *action)
{
    if (!agent_id || !model_name || !action) {
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_check_llm_permission: daemon_security not initialized — "
                      "call daemon_cupolas_init() during startup. DENYING %s/%s (fail-closed).",
                      agent_id, model_name);
        return AIRY_EPERM;
    }

    if (!g_security_ctx.permission_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("LLM permission check: disabled by configuration, DENYING %s/%s (fail-closed)",
                     agent_id, model_name);
        return AIRY_EPERM;
    }

    char resource[256];
    snprintf(resource, sizeof(resource), "llm:%s", model_name);

    int result = AIRY_ERR_PERMISSION_DENIED;
    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strstr(g_security_ctx.acl_table[i].resource, resource) != NULL) {

            if (g_security_ctx.acl_table[i].allowed) {
                result = AIRY_OK;
            }
            break;
        }
    }
    airy_mtx_unlock(&g_security_mutex);

    if (result == AIRY_OK) {
        SVC_LOG_DEBUG("LLM Permission GRANTED: agent=%s model=%s action=%s", agent_id, model_name,
                      action);
    } else {
        SVC_LOG_SECURITY("LLM Permission DENIED (no ACL): agent=%s model=%s action=%s", agent_id,
                         model_name, action);
    }
    return result;
}

int daemon_verify_package_signature(const char *package_path, bool *is_valid,
                                    cupolas_signer_info_t *signer_info)
{
    if (!package_path || !is_valid) {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (signer_info)
        __builtin_memset(signer_info, 0, sizeof(cupolas_signer_info_t));

    *is_valid = false;

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR(
            "daemon_verify_package_signature: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. Marking package UNVERIFIED (fail-closed).");
        *is_valid = false;
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.signature_enabled) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("Signature verification: disabled by configuration, package NOT verified");
        *is_valid = false;
        return AIRY_OK;
    }
    airy_mtx_unlock(&g_security_mutex);

    struct stat st;
    if (stat(package_path, &st) != 0) {
        SVC_LOG_ERROR("Package not found: %s", package_path);
        return AIRY_ERR_NOT_FOUND;
    }

    if (st.st_size == 0) {
        SVC_LOG_ERROR("Package is empty: %s", package_path);
        return AIRY_ERR_INVALID_PARAM;
    }

    if (st.st_size > 512 * 1024 * 1024) {
        SVC_LOG_ERROR("Package exceeds size limit: %s (%lld bytes)", package_path,
                      (long long)st.st_size);
        *is_valid = false;
        return AIRY_OK;
    }

    char sig_path[1024];
    snprintf(sig_path, sizeof(sig_path), "%s.sig", package_path);
    struct stat sig_st;
    if (stat(sig_path, &sig_st) != 0) {
        SVC_LOG_WARN("No signature file found for %s (expected %s), marking as unverified",
                     package_path, sig_path);
        *is_valid = false;
        return AIRY_OK;
    }

    FILE *sig_fp = fopen(sig_path, "rb");
    if (!sig_fp) {
        SVC_LOG_ERROR("Cannot open signature file: %s", sig_path);
        *is_valid = false;
        return AIRY_OK;
    }

    uint8_t signature[256] = {0};
    size_t sig_len = fread(signature, 1, sizeof(signature), sig_fp);
    fclose(sig_fp);

    if (sig_len < 64 || sig_len > 256) {
        SVC_LOG_ERROR("Invalid signature length: %zu bytes (expected 64 for ED25519)", sig_len);
        *is_valid = false;
        return AIRY_OK;
    }

#ifdef AIRY_HAS_OPENSSL
    const char *trusted_keys_dir = getenv("AIRY_TRUSTED_KEYS_DIR");
    if (!trusted_keys_dir) {
        trusted_keys_dir = AIRY_CONFIG_DIR "/trusted_keys";
    }

    DIR *dir = opendir(trusted_keys_dir);
    if (!dir) {
        SVC_LOG_WARN("Trusted keys directory not found: %s, cannot verify signature",
                     trusted_keys_dir);
        *is_valid = false;
        return AIRY_OK;
    }

    FILE *pkg_fp = fopen(package_path, "rb");
    if (!pkg_fp) {
        closedir(dir);
        SVC_LOG_ERROR("Cannot open package file: %s", package_path);
        *is_valid = false;
        return AIRY_OK;
    }

    uint8_t *pkg_data = (uint8_t *)AIRY_MALLOC((size_t)st.st_size);
    if (!pkg_data) {
        fclose(pkg_fp);
        closedir(dir);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate package data buffer");
    }
    size_t pkg_read = fread(pkg_data, 1, (size_t)st.st_size, pkg_fp);
    fclose(pkg_fp);

    if (pkg_read != (size_t)st.st_size) {
        AIRY_FREE(pkg_data);
        closedir(dir);
        SVC_LOG_ERROR("Failed to read entire package: %s", package_path);
        *is_valid = false;
        return AIRY_OK;
    }

    bool verified = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        size_t name_len = strlen(entry->d_name);
        if (name_len < 5 || strcmp(entry->d_name + name_len - 4, ".pem") != 0)
            continue;

        char key_path[1024];
        snprintf(key_path, sizeof(key_path), "%s/%s", trusted_keys_dir, entry->d_name);

        FILE *key_fp = fopen(key_path, "r");
        if (!key_fp)
            continue;

        EVP_PKEY *pkey = PEM_read_PUBKEY(key_fp, NULL, NULL, NULL);
        fclose(key_fp);

        if (!pkey)
            continue;

        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
            EVP_PKEY_free(pkey);
            continue;
        }

        EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
        if (!md_ctx) {
            EVP_PKEY_free(pkey);
            continue;
        }

        if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
            EVP_MD_CTX_free(md_ctx);
            EVP_PKEY_free(pkey);
            continue;
        }

        int verify_result = EVP_DigestVerify(md_ctx, signature, sig_len, pkg_data, pkg_read);

        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);

        if (verify_result == 1) {
            verified = true;
            if (signer_info) {
                char *dot = strchr(entry->d_name, '.');
                size_t id_len = dot ? (size_t)(dot - entry->d_name) : name_len;
                if (id_len >= sizeof(signer_info->key_id))
                    id_len = sizeof(signer_info->key_id) - 1;
                __builtin_memcpy(signer_info->key_id, entry->d_name, id_len);
                signer_info->key_id[id_len] = '\0';
                signer_info->algorithm = AIRY_STRDUP("ED25519");
            }
            SVC_LOG_INFO("Package signature VERIFIED (ED25519): %s with key %s", package_path,
                         entry->d_name);
            break;
        }
    }

    closedir(dir);
    AIRY_FREE(pkg_data);

    *is_valid = verified;
    if (!verified) {
        SVC_LOG_SECURITY("Package signature INVALID: %s (no trusted key matched)", package_path);
    }
    return AIRY_OK;

#else
    SVC_LOG_WARN("OpenSSL not available: cannot perform ED25519 verification for %s, "
                 "marking as unverified (size=%lld bytes)",
                 package_path, (long long)st.st_size);
    *is_valid = false;
    return AIRY_ENOTSUP;
#endif
}

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
        SVC_LOG_ERROR(
            "daemon_store_credential: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. DENYING credential storage (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.vault_enabled || !g_security_ctx.vault) {
        airy_mtx_unlock(&g_security_mutex);
        AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "vault is disabled or unavailable");
    }

    cupolas_vault_t *vault = g_security_ctx.vault;
    int rc = cupolas_vault_store(vault, cred_id, cred_type, data, data_len, NULL);
    if (rc != 0) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("daemon_store_credential: cupolas_vault_store FAILED for cred_id=%s (rc=%d)",
                      cred_id, rc);
        return AIRY_ERR_UNKNOWN;
    }

    /* Keep the owner_agent_id ACL semantics: the storer is the default
     * authorizer (read/write/delete); other agents must be explicitly granted
     * access via grant_access (vault denies by default, fail-closed). */
    const char *owner = agent_id ? agent_id : "system";
    int acl_rc = cupolas_vault_grant_access(vault, cred_id, owner,
                                            CUPOLAS_VAULT_OP_READ | CUPOLAS_VAULT_OP_WRITE |
                                                CUPOLAS_VAULT_OP_DELETE,
                                            0);
    if (acl_rc != 0) {
        SVC_LOG_WARN("daemon_store_credential: grant_access FAILED for cred_id=%s owner=%s (rc=%d)",
                     cred_id, owner, acl_rc);
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
                      "call daemon_cupolas_init() during startup. DENYING credential retrieval "
                      "(fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    if (!g_security_ctx.vault_enabled || !g_security_ctx.vault) {
        airy_mtx_unlock(&g_security_mutex);
        AIRY_ERROR(AIRY_ERR_NOT_SUPPORTED, "vault is disabled or unavailable");
    }

    /* Keep the "system" exemption semantics: system-level calls carry no
     * agent constraint and are left to the vault's ACL; all other requests
     * are scoped by agent_id (fail-closed). */
    const char *requester = agent_id ? agent_id : "system";

    size_t buf_len = *data_len;
    int rc = cupolas_vault_retrieve(g_security_ctx.vault, cred_id, requester, data, &buf_len);
    if (rc != 0) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_WARN("daemon_retrieve_credential: cupolas_vault_retrieve FAILED for cred_id=%s "
                     "agent=%s (rc=%d) — %s",
                     cred_id, requester, rc,
                     (rc == (int)cupolas_ERR_INVALID_PARAM || rc == (int)cupolas_ERR_NULL_POINTER) ?
                         "credential not found or access denied" :
                         "internal error");
        return (rc == (int)cupolas_ERR_NULL_POINTER) ? AIRY_ERR_NOT_FOUND :
                                                       AIRY_ERR_PERMISSION_DENIED;
    }

    *data_len = buf_len;
    airy_mtx_unlock(&g_security_mutex);
    return AIRY_OK;
}

cupolas_vault_t *daemon_security_get_vault(void)
{
    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    cupolas_vault_t *vault = g_security_ctx.vault;
    airy_mtx_unlock(&g_security_mutex);
    return vault;
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
        SVC_LOG_ERROR(
            "daemon_audit_log_event: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. Audit event DROPPED (fail-closed).");
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

int daemon_security_add_acl_rule(const char *agent_id, const char *resource, bool allowed)
{
    if (!agent_id || !resource || agent_id[0] == '\0' || resource[0] == '\0') {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (strlen(agent_id) >= sizeof(g_security_ctx.acl_table[0].agent_id) ||
        strlen(resource) >= sizeof(g_security_ctx.acl_table[0].resource)) {
        SVC_LOG_ERROR("ACL rule rejected: agent_id or resource too long (agent=%s resource=%s)",
                      agent_id, resource);
        return AIRY_ERR_INVALID_PARAM;
    }

    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);

    if (!g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR(
            "daemon_security_add_acl_rule: daemon_security not initialized — "
            "call daemon_cupolas_init() during startup. DENYING ACL rule add (fail-closed).");
        return AIRY_ERR_STATE_ERROR;
    }

    for (size_t i = 0; i < g_security_ctx.acl_count; i++) {
        if (strcmp(g_security_ctx.acl_table[i].agent_id, agent_id) == 0 &&
            strcmp(g_security_ctx.acl_table[i].resource, resource) == 0) {
            g_security_ctx.acl_table[i].allowed = allowed;
            airy_mtx_unlock(&g_security_mutex);
            SVC_LOG_DEBUG("ACL rule updated: agent=%s resource=%s allowed=%d", agent_id, resource,
                          allowed ? 1 : 0);
            return AIRY_OK;
        }
    }

    if (g_security_ctx.acl_count >= MAX_ACL_ENTRIES) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_ERROR("ACL table full (max=%d), cannot add rule for agent=%s resource=%s",
                      MAX_ACL_ENTRIES, agent_id, resource);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    acl_entry_t *entry = &g_security_ctx.acl_table[g_security_ctx.acl_count];
    snprintf(entry->agent_id, sizeof(entry->agent_id), "%s", agent_id);
    snprintf(entry->resource, sizeof(entry->resource), "%s", resource);
    entry->operations = 0xFFFFFFFF;
    entry->allowed = allowed;
    g_security_ctx.acl_count++;
    airy_mtx_unlock(&g_security_mutex);

    SVC_LOG_INFO("ACL rule added: agent=%s resource=%s allowed=%d (count=%zu/%d)", agent_id,
                 resource, allowed ? 1 : 0, g_security_ctx.acl_count, MAX_ACL_ENTRIES);
    return AIRY_OK;
}

int daemon_security_load_rules_file(const char *path)
{
    if (!path || !path[0])
        return AIRY_ERR_INVALID_PARAM;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return AIRY_ERR_NOT_FOUND;
    fclose(fp);

    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_ERR_OUT_OF_MEMORY;
    if (yaml_parse_file(doc, path) != 0) {
        SVC_LOG_ERROR("daemon_security: failed to parse permission rules file: %s (%s)", path,
                      yaml_get_error(doc) ? yaml_get_error(doc) : "parse error");
        yaml_destroy(doc);
        return AIRY_ERR_PARSE_ERROR;
    }

    struct yaml_node *root = yaml_root(doc);
    struct yaml_node *rules = root ? yaml_get(root, "rules") : NULL;
    if (!rules) {
        SVC_LOG_WARN("daemon_security: permission rules file has no 'rules' section: %s", path);
        yaml_destroy(doc);
        return AIRY_ERR_PARSE_ERROR;
    }

    size_t total = yaml_size(rules);
    size_t added = 0;
    for (size_t i = 0; i < total; i++) {
        struct yaml_node *r = yaml_get_index(rules, i);
        if (!r)
            continue;
        const char *agent = yaml_as_string(yaml_get(r, "agent"), NULL);
        const char *tool = yaml_as_string(yaml_get(r, "tool"), NULL);
        const char *effect = yaml_as_string(yaml_get(r, "effect"), "deny");
        if (!agent || !tool || !effect || agent[0] == '\0' || tool[0] == '\0')
            continue;
        bool allowed = (strcmp(effect, "allow") == 0);
        if (daemon_security_add_acl_rule(agent, tool, allowed) == AIRY_OK)
            added++;
    }
    yaml_destroy(doc);

    SVC_LOG_INFO("daemon_security: loaded %zu/%zu permission rules from %s", added, total, path);
    return AIRY_OK;
}
