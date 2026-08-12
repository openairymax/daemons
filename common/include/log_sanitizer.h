/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file log_sanitizer.h
 * @brief Log sanitization filter - prevents sensitive info leaking to logs.
 *
 * Features:
 * 1. Auto-detects and masks sensitive fields (API keys, passwords, tokens)
 * 2. Supports custom sensitive-field patterns
 * 3. Thread-safe design
 * 4. Zero-copy mode (performance optimization)
 */

#ifndef AIRY_RT_LOG_SANITIZER_H
#define AIRY_RT_LOG_SANITIZER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Sensitive-field pattern. */
typedef struct {
    const char *pattern;
    const char *replacement;
} sensitive_field_t;

/**
 * @brief Initialize the log sanitizer.
 * @param max_fields Max number of sensitive fields
 */
void log_sanitizer_init(size_t max_fields);

/** @brief Destroy the log sanitizer. */
void log_sanitizer_destroy(void);

/**
 * @brief Add a sensitive-field pattern.
 * @param pattern Matching pattern
 * @param replacement Replacement string (NULL = default "***")
 * @return true on success, false on failure
 */
bool log_sanitizer_add_pattern(const char *pattern, const char *replacement);

/**
 * @brief Sanitize a log message.
 * @param message Original message
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Sanitized message length, -1 on failure
 */
int log_sanitize(const char *message, char *buffer, size_t buffer_size);

/**
 * @brief Sanitize a log message (dynamically-allocated version).
 * @param message Original message
 * @return Sanitized message (caller frees), NULL on failure
 */
char *log_sanitize_dup(const char *message);

/**
 * @brief Check whether a message contains sensitive info.
 * @param message Message
 * @return true if it contains sensitive info, false otherwise
 */
bool log_contains_sensitive(const char *message);

/**
 * @brief Get the default sensitive-field list.
 * @return Sensitive-field array
 */
const sensitive_field_t *log_get_default_patterns(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LOG_SANITIZER_H */
