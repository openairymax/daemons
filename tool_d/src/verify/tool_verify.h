/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_verify.h
 * @brief 完成判据复核（0.1.21 C-17）：tool.verify 的实现入口。
 *
 * 只读：不注册工具、不写工作区、不改审批状态；由 agent_d 在终局前
 * 经 daemon 间 RPC 调用。wire 契约见 commons/include/airy_verify.h。
 */

#ifndef AIRY_RT_TOOL_VERIFY_H
#define AIRY_RT_TOOL_VERIFY_H

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 复核一组判据并返回逐项证据。
 *
 * @param criteria   判据数组（非数组/空数组按失败回执，不返回 NULL）
 * @param timeout_ms 单条命令/连接预算（<=0 取默认）
 * @return 应答对象（所有权移交调用方）；仅 OOM 返回 NULL
 */
cJSON *tool_verify_run(const cJSON *criteria, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_VERIFY_H */
