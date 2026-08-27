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
 *
 * Phase 2.3a split: this file keeps module init/shutdown, shared global
 * state and input sanitization; the other domains were split out:
 * - ACL permission checks + rule management -> daemon_security_acl.c
 * - package signature verification          -> daemon_security_signature.c
 * - vault credentials + audit + status      -> daemon_security_vault.c
 * Shared state is declared in daemon_security_internal.h (internal to this
 * static lib, not public API).
 */

#include "daemon_security_internal.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* pthread.h provided by platform.h — no direct pthread include (CROSS-01) */
/* Internal state structure (legacy unused descriptor, kept for compat) */
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
#include "cupolas_vault.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAEMON_VAULT_ID "agentrt"
#define DAEMON_VAULT_PASSWORD_ENV "AIRY_VAULT_PASSWORD"

daemon_security_ctx_t g_security_ctx = {0};

airy_mtx_t g_security_mutex; /* CROSS-01: initialized via airy_mtx_init() */
/* 三态惰性初始化（P1-3）：0=未初始化，2=初始化中，1=就绪。
 * 对齐 hall_writer/gateway_hall_store 的 CAS 范式，杜绝并发重复
 * pthread_mutex_init（UB）。 */
static atomic_int g_security_mutex_ready = 0;

void ensure_mutex_initialized(void)
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
    (void)error; /* error reporting via SVC_LOG; out-param reserved */
    ensure_mutex_initialized();
    airy_mtx_lock(&g_security_mutex);
    if (g_security_ctx.initialized) {
        airy_mtx_unlock(&g_security_mutex);
        SVC_LOG_INFO("Daemon security: already initialized");
        return 0;
    }

    AIRY_MEMSET(&g_security_ctx, 0, sizeof(g_security_ctx));
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
