/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_audit.h
 * @brief D-9 任务实体审计留痕：风险分级、参数脱敏、输出摘要、沙箱标记。
 *
 * 作业记录（job）是任务实体的单一真相源；本模块只提供无状态的纯函数，
 * 由 job/RPC 层在提交与收尾时调用，保证落盘记录可还原「命令、退出码、
 * 耗时、审批人」，同时不把敏感参数值写进审计文件。
 */

#ifndef AIRY_RT_TOOL_AUDIT_H
#define AIRY_RT_TOOL_AUDIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 风险分级：0 = 普通，1 = 高危（需审批或显式 allow_dangerous）。 */
typedef enum {
    TOOL_AUDIT_RISK_LOW = 0,
    TOOL_AUDIT_RISK_HIGH = 1,
} tool_audit_risk_t;

/**
 * @brief 判定本次调用风险。
 * @param tool_id     工具 id（shell_run / tool_create 为固有高危）
 * @param params_json 参数 JSON（命中包管理/服务管理命令模式亦为高危）
 * @param reason      命中原因输出缓冲（可 NULL）
 * @param cap         reason 容量
 * @return TOOL_AUDIT_RISK_LOW / TOOL_AUDIT_RISK_HIGH
 */
tool_audit_risk_t tool_audit_risk(const char *tool_id, const char *params_json, char *reason,
                                  size_t cap);

/**
 * @brief 生成脱敏参数摘要（单行、定长、敏感键值置掩码）。
 * @param params_json 原始参数 JSON（可 NULL）
 * @param buf         输出缓冲
 * @param cap         容量（>=1）
 *
 * 敏感键（password/token/secret/api_key/authorization/cookie 等）的值被
 * 就地掩码为 "***"；JSON 解析失败时退化为原始串截断，仍不保留换行。
 */
void tool_audit_mask(const char *params_json, char *buf, size_t cap);

/**
 * @brief 生成输出摘要（单行、截断；用于作业记录，不保存完整输出）。
 * @param text 原始输出（可 NULL）
 * @param buf  输出缓冲
 * @param cap  容量（>=1）
 */
void tool_audit_sum(const char *text, char *buf, size_t cap);

/**
 * @brief 当前沙箱层级标记：landlock / degraded / off。
 * @return 静态字符串（不释放）
 */
const char *tool_audit_sbox(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_AUDIT_H */
