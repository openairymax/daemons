// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file compat.h
 * @brief 兼容性定义兼容层
 *
 * 本文件是 agentrt/commons/utils/compat 的兼容层，提供向后兼容的 API。
 * 新代码应直接使用 #include <compat.h>
 *
 * @see agentrt/commons/utils/compat/include/compat.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_COMPAT_H
#define AIRY_RT_DAEMON_COMMON_COMPAT_H

#include <compat.h>
#include <stdlib.h>
#include "memory_compat.h"

/* ==================== 额外的兼容性别名 ==================== */

/* 静态断言兼容 */
#ifndef AIRY_STATIC_ASSERT
#define AIRY_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* 调试断点兼容 */
#ifndef AIRY_DEBUG_BREAK
#define AIRY_DEBUG_BREAK() airy_debug_break()
#endif

/* 安全字符串函数别名 */
#define airy_strlcpy_safe airy_strncpy_safe

/* ==================== 额外的位操作宏 ==================== */

/**
 * @brief 位掩码生成
 */
#define AIRY_BIT_MASK(bit) (1U << (bit))

/**
 * @brief 字节掩码生成
 */
#define AIRY_BYTE_MASK(byte) (0xFFU << ((byte) * 8))

/**
 * @brief 字提取
 */
#define AIRY_GET_BYTE(value, byte) (((value) >> ((byte) * 8)) & 0xFFU)

/**
 * @brief 字设置
 */
#define AIRY_SET_BYTE(value, byte, val) \
    (((value) & ~AIRY_BYTE_MASK(byte)) | (((val) & 0xFFU) << ((byte) * 8)))

/* ==================== 编译器特定扩展 ==================== */

/**
 * @brief 函数签名（用于调试）
 */
#if defined(AIRY_COMPILER_GCC) || defined(AIRY_COMPILER_CLANG)
#define AIRY_FUNC_SIGNATURE __PRETTY_FUNCTION__
#elif defined(AIRY_COMPILER_MSVC)
#define AIRY_FUNC_SIGNATURE __FUNCSIG__
#else
#define AIRY_FUNC_SIGNATURE __func__
#endif

/* ==================== 分支预测辅助宏 ==================== */

/**
 * @brief 检查指针是否有效（非空且可读）
 */
#define AIRY_LIKELY_VALID(ptr) AIRY_LIKELY((ptr) != NULL)
#define AIRY_UNLIKELY_NULL(ptr) AIRY_UNLIKELY((ptr) == NULL)

/* ==================== 资源管理辅助 ==================== */

/**
 * @brief 自动清理属性
 */
#if defined(AIRY_COMPILER_GCC) || defined(AIRY_COMPILER_CLANG)
#define AIRY_CLEANUP(func) __attribute__((cleanup(func)))
#else
#define AIRY_CLEANUP(func)
#endif

/**
 * @brief 自动释放宏
 */
#define AIRY_AUTO_FREE __attribute__((cleanup(airy_auto_free_helper)))
static inline void airy_auto_free_helper(void **ptr)
{
    if (ptr && *ptr) {
        AIRY_FREE(*ptr);
        *ptr = NULL;
    }
}

/* ==================== 编译时字符串操作 ==================== */

/**
 * @brief 字符串化宏
 */
#define AIRY_STRINGIFY(x) #x
#define AIRY_TOSTRING(x) AIRY_STRINGIFY(x)

/**
 * @brief 连接宏
 */
#define AIRY_CONCAT(a, b) a##b
#define AIRY_CONCAT3(a, b, c) a##b##c

/* ==================== 类型安全宏 ==================== */

/**
 * @brief 类型安全的数组大小
 */
#define AIRY_ARRAY_SIZE_SAFE(arr) (sizeof(arr) / sizeof((arr)[0]) + sizeof(typeof(arr[0])) * 0)

/**
 * @brief 类型检查
 */
#define AIRY_TYPE_CHECK(type, expr) ((type){0}, (expr))

/* ==================== 编译器版本检查 ==================== */

/**
 * @brief 检查 GCC 版本是否至少为指定版本
 */
#if defined(AIRY_COMPILER_GCC)
#define AIRY_GCC_VERSION_AT_LEAST(major, minor) \
    (__GNUC__ > (major) || (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor)))
#else
#define AIRY_GCC_VERSION_AT_LEAST(major, minor) 0
#endif

/**
 * @brief 检查 Clang 版本是否至少为指定版本
 */
#if defined(AIRY_COMPILER_CLANG)
#define AIRY_CLANG_VERSION_AT_LEAST(major, minor) \
    (__clang_major__ > (major) || (__clang_major__ == (major) && __clang_minor__ >= (minor)))
#else
#define AIRY_CLANG_VERSION_AT_LEAST(major, minor) 0
#endif

/* ==================== C11 特性检测 ==================== */

/**
 * @brief 检查是否支持 _Generic
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_GENERIC 1
#else
#define AIRY_HAS_GENERIC 0
#endif

/**
 * @brief 检查是否支持 _Static_assert
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_STATIC_ASSERT 1
#else
#define AIRY_HAS_STATIC_ASSERT 0
#endif

/**
 * @brief 检查是否支持匿名结构体/联合体
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define AIRY_HAS_ANONYMOUS 1
#else
#define AIRY_HAS_ANONYMOUS 0
#endif

#endif /* AIRY_RT_DAEMON_COMMON_COMPAT_H */
