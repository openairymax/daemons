/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 *
 * daemon_security.h - Daemon Layer Security Core (mechanism)
 *
 * Design Principles:
 * - Security by Default: All daemon services must use these security features
 * - Zero Trust: Every request must be validated and sanitized
 * - Defense in Depth: Multiple security layers for comprehensive protection
 * - Audit Trail: All operations must be logged for compliance
 */

#ifndef DAEMON_SECURITY_H
#define DAEMON_SECURITY_H

#include "daemon_errors.h"
#include "error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * External Dependency Guard (E-1 security built-in + E-3 resource
 * determinism + S-2 hierarchical decomposition)
 *
 * daemon_security uses an independent production-grade security
 * implementation and does not depend on the cupolas library. Security
 * features (credential vault / audit logging / input sanitization / ACL)
 * are all really implemented in the security domain sources.
 *
 * Type definitions reuse the canonical header under commons/utils/types/:
 * - cupolas_vault_cred_type.h: credential-type enum
 *
 * Credential storage is a policy concern: the mechanism core never links
 * the concrete vault backend. A daemon_vault_provider_t is injected at
 * init time (see below) and owns the vault identity, passphrase source
 * and backend. With no provider the core runs fail-closed (storage off).
 *
 * Design decisions follow ARCHITECTURAL_PRINCIPLES.md:
 * - K-4 zero trust: all security operations fail closed, deny by default
 * - E-6 traceable errors: full audit logs
 * - S-2 hierarchical decomposition: security types separated from impl
 */
#include "cupolas_vault_cred_type.h"
#include "sanitize_level.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Credential vault provider seam (mechanism/policy separation)
 *
 * The daemon security core (svc_common) must not link the cupolas product
 * library. Credential storage is therefore delegated to a policy-side
 * provider injected via daemon_security_config_t.vault_provider. The
 * provider owns the vault identity, passphrase source and concrete
 * backend; the core only sees this abstract operations table. A handle
 * returned by open() is opaque to the core and is passed back into
 * store()/retrieve()/close().
 *
 * Each operation returns AIRY_OK on success and a negative airy_err_t on
 * failure. Providers should return AIRY_ERR_NOT_FOUND when a credential
 * is absent and AIRY_ERR_PERMISSION_DENIED when the requester lacks
 * access; the core passes these through unchanged.
 */
typedef struct daemon_vault_provider {
    airy_err_t (*open)(void **out_handle); /**< Open backend; *out_handle set on success */
    void (*close)(void *handle); /**< Close a handle from open(); NULL-safe */
    airy_err_t (*store)(void *handle, const char *cred_id,
                        cupolas_vault_cred_type_t cred_type, const uint8_t *data, size_t data_len,
                        const char *owner); /**< Store cred, grant owner full access */
    airy_err_t (*retrieve)(void *handle, const char *cred_id, const char *requester, uint8_t *data,
                           size_t *data_len); /**< Retrieve cred for requester */
} daemon_vault_provider_t;

/**
 * @brief Daemon security configuration structure
 *
 * Provides unified security configuration for all daemon services:
 * - Input sanitization level
 * - Permission checking mode
 * - Audit logging settings
 */
typedef struct daemon_security_config {
    /* Sanitizer configuration */
    sanitize_level_t sanitize_level; /**< Input sanitization level */
    const char *sanitizer_rules_path; /**< Path to sanitizer rules file */

    /* Permission configuration */
    const char *permission_rules_path; /**< Path to permission rules file */
    bool enable_permission_cache; /**< Enable permission result caching */
    /* Vault configuration (policy backend injected by the caller) */
    bool enable_vault; /**< Enable secure credential storage */
    const daemon_vault_provider_t *vault_provider; /**< Credential backend (NULL disables vault) */
    /* Audit configuration */
    bool enable_audit_logging; /**< Enable audit logging */
    const char *audit_log_dir; /**< Directory for audit logs */
} daemon_security_config_t;

/**
 * @brief Initialize daemon security layer
 *
 * This function initializes the security core for daemon usage.
 * It should be called once during daemon startup before any service initialization.
 *
 * @param[in] config Security configuration (NULL for defaults)
 * @param[out] error Optional error code output
 * @return 0 on success, negative on failure
 *
 * @note Thread-safe: Safe to call from main thread only during initialization
 * @reentrant No
 *
 * @ownership config: caller retains ownership, may be NULL
 * @ownership error: caller provides buffer, function writes to it
 *
 * @details
 * This function performs the following initializations:
 * 1. Initializes the security core state
 * 2. Loads sanitizer rules from configuration
 * 3. Loads permission rules from configuration
 * 4. Opens the injected vault provider (if enabled)
 * 5. Configures audit logging
 *
 * Example usage:
 * @code
 * daemon_security_config_t sec_config = {
 *     .sanitize_level = SANITIZE_LEVEL_HIGH,
 *     .sanitizer_rules_path = AIRY_CONFIG_DIR "/cupolas/sanitizer_rules.yaml",
 *     .permission_rules_path = AIRY_CONFIG_DIR "/cupolas/permission_rules.yaml",
 *     .enable_permission_cache = true,
 *     .enable_vault = true,
 *     .vault_provider = &my_vault_provider,
 *     .enable_audit_logging = true,
 *     .audit_log_dir = AIRY_LOG_DIR "/cupolas"
 * };
 *
 * airy_err_t error;
 * int ret = daemon_security_init(&sec_config, &error);
 * if (ret != 0) {
 *     fprintf(stderr, "Security init failed: %s\n", error.message);
 * }
 * @endcode
 */
int daemon_security_init(const daemon_security_config_t *config, airy_err_t *error);

/**
 * @brief Shutdown daemon security layer
 *
 * Cleans up the security core state. Should be called during daemon shutdown.
 *
 * @note Thread-safe: Safe to call from main thread only during shutdown
 * @reentrant No
 */
void daemon_security_shutdown(void);

/**
 * @brief Sanitize input string for LLM service requests
 *
 * Sanitizes user input to prevent injection attacks before passing to LLM providers.
 * Uses the configured sanitization level from daemon_security_init().
 *
 * @param[in] input Raw input string from user or external source
 * @param[out] output Sanitized output buffer
 * @param[in] output_size Size of output buffer
 * @return 0 on success, negative on failure
 *
 * @note Thread-safe: Safe to call from multiple threads concurrently
 * @reentrant Yes
 *
 * @ownership input: caller retains ownership
 * @ownership output: caller provides buffer, function writes to it
 *
 * @details
 * Sanitizes against:
 * - SQL injection attacks
 * - XSS (Cross-Site Scripting)
 * - Command injection attacks
 * - Path traversal attacks
 * - Special character attacks
 *
 * Example usage:
 * @code
 * char sanitized[4096];
 * const char* user_input = "SELECT * FROM users WHERE id=1 OR 1=1";
 *
 * if (daemon_sanitize_llm_input(user_input, sanitized, sizeof(sanitized)) == 0) {
 *     // Use sanitized input for LLM call
 * } else {
 *     // Handle sanitization failure
 * }
 * @endcode
 */
int daemon_sanitize_llm_input(const char *input, char *output, size_t output_size);

/**
 * @brief Sanitize tool execution parameters
 *
 * Sanitizes tool name and parameters to prevent command injection.
 * Should be called before any tool execution in the tool_d service.
 *
 * @param[in] tool_name Tool name to sanitize
 * @param[in] params Tool parameters (JSON format)
 * @param[out] sanitized_tool Output buffer for sanitized tool name
 * @param[in] tool_buf_size Size of tool output buffer
 * @param[out] sanitized_params Output buffer for sanitized parameters
 * @param[in] param_buf_size Size of parameters output buffer
 * @return 0 on success, negative on failure
 *
 * @note Thread-safe: Safe to call from multiple threads concurrently
 * @reentrant Yes
 *
 * @ownership All parameters: caller retains ownership or provides buffers as appropriate
 *
 * @details
 * Performs comprehensive sanitization including:
 * - Tool name validation against whitelist
 * - Parameter type validation
 * - Command argument escaping
 * - Path traversal prevention
 */
int daemon_sanitize_tool_params(const char *tool_name, const char *params, char *sanitized_tool,
                                size_t tool_buf_size, char *sanitized_params,
                                size_t param_buf_size);

/**
 * @brief Check tool execution permission
 *
 * Verifies that the requesting agent has permission to execute a specific tool.
 * Should be called before tool execution in the tool_d service.
 *
 * @param[in] agent_id Agent identifier requesting access
 * @param[in] tool_name Name of the tool to execute
 * @param[in] action Action type ("execute", "read", "write")
 * @return 1 if allowed, 0 if denied
 *
 * @note Thread-safe: Safe to call from multiple threads concurrently
 * @reentrant Yes
 *
 * @ownership All parameters: caller retains ownership
 *
 * @details
 * Checks against configured permission rules:
 * - Agent identity verification
 * - Tool-specific permissions
 * - Rate limiting checks
 * - Context-aware authorization
 *
 * Example usage:
 * @code
 * if (daemon_check_tool_permission("agent-001", "file_read", "execute") != 0) {
 *     return AIRY_ERR_PERMISSION_DENIED;
 * }
 * // Proceed with tool execution
 * @endcode
 */
int daemon_check_tool_permission(const char *agent_id, const char *tool_name, const char *action);

/**
 * @brief Log audit event for daemon operation
 *
 * Records an audit event for compliance tracking.
 * Called after tool approval decisions (grant, deny, safety guard).
 *
 * @param[in] service_name Service name (currently the tool approval domain)
 * @param[in] operation Operation performed
 * @param[in] resource Resource accessed
 * @param[in] result Result of operation (success/failure)
 * @param[in] agent_id Agent performing the operation
 * @return 0 on success, negative on failure
 *
 * @note Thread-safe: Safe to call from multiple threads concurrently
 * @reentrant Yes
 *
 * @ownership All parameters: caller retains ownership
 *
 * @details
 * Logs include:
 * - Timestamp
 * - Service identification
 * - Operation details
 * - Resource information
 * - Result status
 * - Agent identification
 */
int daemon_audit_log_event(const char *service_name, const char *operation, const char *resource,
                           int result, const char *agent_id);

/**
 * @brief Register an ACL permission rule
 *
 * Adds an entry to the in-memory ACL table used by daemon_check_tool_permission().
 * This is the programmatic equivalent of loading a permission_rules.yaml
 * entry, and is intended for use by:
 *   - Test harnesses that need to provision ACL entries before running
 *   - Runtime administrators who need to grant/deny permissions dynamically
 *   - Bootstrap paths that provision permissions before the rule-file loader runs
 *
 * The ACL table is fail-closed: daemon_check_tool_permission() returns DENY
 * for any (agent_id, tool_name) pair without a matching allowed=true entry.
 * Without calling this function (or loading a rules file), all permission
 * checks will fail.
 *
 * @param[in] agent_id Agent identifier (max 63 chars, NUL-terminated)
 * @param[in] resource Resource name: tool_name for tool checks
 *                     (max 127 chars, NUL-terminated)
 * @param[in] allowed  true to grant permission, false to explicitly deny
 * @return 0 on success, negative on failure (AIRY_ERR_INVALID_PARAM / AIRY_ERR_OUT_OF_MEMORY)
 *
 * @note Thread-safe: Safe to call from multiple threads concurrently
 * @reentrant Yes
 *
 * @ownership All parameters: caller retains ownership, function copies internally
 *
 * @example
 * @code
 * // Grant agent-001 permission to execute file_read
 * daemon_security_add_acl_rule("agent-001", "file_read", true);
 * // Explicitly deny restricted-agent from shell_exec
 * daemon_security_add_acl_rule("restricted-agent", "shell_exec", false);
 * @endcode
 */
int daemon_security_add_acl_rule(const char *agent_id, const char *resource, bool allowed);

#ifdef __cplusplus
}
#endif

#endif /* DAEMON_SECURITY_H */
