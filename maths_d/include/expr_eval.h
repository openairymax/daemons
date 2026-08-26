/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file expr_eval.h
 * @brief 数学表达式求值引擎（递归下降解析器）公共接口。
 *
 * 从 maths_service.c 拆分（n11 大文件模块化）：纯 C 数值表达式求值，
 * 覆盖建议稿第一/二级（基础算术 + 初等函数）。零外部依赖、零代码执行，
 * 表达式长度与解析深度硬限制，NaN/Inf 显式上报。
 *
 * @see maths_service.h 数学外挂服务核心（调用本引擎）
 */

#ifndef AIRY_RT_MATHS_D_EXPR_EVAL_H
#define AIRY_RT_MATHS_D_EXPR_EVAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 表达式求值。expr 为数学表达式，out_result 指向结果缓冲。
 * @return 0 成功；非 0 失败（err_msg 填充可读错误）。
 */
int maths_d_eval(const char *expr, double *out_result, char *err_msg,
                 size_t err_msg_size);

/**
 * @brief 表达式字符集白名单预检（防注入）。
 * @return 1 字符集合法；0 含非法字符。
 */
int maths_charset_ok(const char *expr);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MATHS_D_EXPR_EVAL_H */
