/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file compat.h
 * @brief Backward-compatibility layer.
 *
 * Compat layer for agentrt/commons/utils/compat providing backward
 * compatible APIs. New code should use #include <compat.h> directly.
 *
 * @see agentrt/commons/utils/compat/compat.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_COMPAT_H
#define AIRY_RT_DAEMON_COMMON_COMPAT_H

#include <compat.h>
#include <stdlib.h>
#include "airy_memory.h"


#ifndef AIRY_STATIC_ASSERT
#define AIRY_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif


#ifndef AIRY_DEBUG_BREAK
#define AIRY_DEBUG_BREAK() airy_debug_break()
#endif


#define airy_strlcpy_safe airy_strncpy_safe


/** @brief Generate a bit mask. */
#define AIRY_BIT_MASK(bit) (1U << (bit))

/** @brief Generate a byte mask. */
#define AIRY_BYTE_MASK(byte) (0xFFU << ((byte) * 8))

/** @brief Extract a byte. */
#define AIRY_GET_BYTE(value, byte) (((value) >> ((byte) * 8)) & 0xFFU)

/** @brief Set a byte. */
#define AIRY_SET_BYTE(value, byte, val) \
    (((value) & ~AIRY_BYTE_MASK(byte)) | (((val) & 0xFFU) << ((byte) * 8)))


/** @brief Function signature (for debugging). */
#if defined(AIRY_COMPILER_GCC) || defined(AIRY_COMPILER_CLANG)
#define AIRY_FUNC_SIGNATURE __PRETTY_FUNCTION__
#elif defined(AIRY_COMPILER_MSVC)
#define AIRY_FUNC_SIGNATURE __FUNCSIG__
#else
#define AIRY_FUNC_SIGNATURE __func__
#endif


/** @brief Check that a pointer is valid (non-NULL). */
#define AIRY_LIKELY_VALID(ptr) AIRY_LIKELY((ptr) != NULL)
#define AIRY_UNLIKELY_NULL(ptr) AIRY_UNLIKELY((ptr) == NULL)


/** @brief Automatic cleanup attribute. */
#if defined(AIRY_COMPILER_GCC) || defined(AIRY_COMPILER_CLANG)
#define AIRY_CLEANUP(func) __attribute__((cleanup(func)))
#else
#define AIRY_CLEANUP(func)
#endif

/** @brief Automatic free macro. */
#define AIRY_AUTO_FREE __attribute__((cleanup(airy_auto_free_helper)))
static inline void airy_auto_free_helper(void **ptr)
{
    if (ptr && *ptr) {
        AIRY_FREE(*ptr);
        *ptr = NULL;
    }
}


/** @brief Stringize macro. */
#define AIRY_STRINGIFY(x) #x
#define AIRY_TOSTRING(x) AIRY_STRINGIFY(x)

/** @brief Concatenation macro. */
#define AIRY_CONCAT(a, b) a##b
#define AIRY_CONCAT3(a, b, c) a##b##c


/** @brief Type-safe array size. */
#define AIRY_ARRAY_SIZE_SAFE(arr) (sizeof(arr) / sizeof((arr)[0]) + sizeof(typeof(arr[0])) * 0)

/** @brief Type check. */
#define AIRY_TYPE_CHECK(type, expr) ((type){0}, (expr))


/** @brief Check GCC version is at least the given version. */
#if defined(AIRY_COMPILER_GCC)
#define AIRY_GCC_VERSION_AT_LEAST(major, minor) \
    (__GNUC__ > (major) || (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor)))
#else
#define AIRY_GCC_VERSION_AT_LEAST(major, minor) 0
#endif

/** @brief Check Clang version is at least the given version. */
#if defined(AIRY_COMPILER_CLANG)
#define AIRY_CLANG_VERSION_AT_LEAST(major, minor) \
    (__clang_major__ > (major) || (__clang_major__ == (major) && __clang_minor__ >= (minor)))
#else
#define AIRY_CLANG_VERSION_AT_LEAST(major, minor) 0
#endif


/** @brief Check _Generic support. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_GENERIC 1
#else
#define AIRY_HAS_GENERIC 0
#endif

/** @brief Check _Static_assert support. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_STATIC_ASSERT 1
#else
#define AIRY_HAS_STATIC_ASSERT 0
#endif

/** @brief Check anonymous struct/union support. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_ANONYMOUS 1
#else
#define AIRY_HAS_ANONYMOUS 0
#endif

#endif /* AIRY_RT_DAEMON_COMMON_COMPAT_H */
