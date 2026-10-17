/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_maths_internal.h
 * @brief Built-in maths tools, delegating to the maths_d service.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_MATHS_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_MATHS_INTERNAL_H

#include "builtin/builtin_limits_internal.h"
#include "builtin/builtin_result_internal.h"

/* maths_d 端点 SSoT：RPC 调用（builtin_maths.c）与依赖声明
 * （service_builtin.c, D8）共用同一 socket 名，杜绝两处漂移。 */
#define MATHS_D_SOCKET_NAME "maths.sock"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in maths tools (builtin_maths.c) — 委托 maths_d 数学外挂服务 */
int maths_eval_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int maths_stats_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int maths_plot_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_MATHS_INTERNAL_H */
