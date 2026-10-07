/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_proto_types.h
 * @brief gateway_d 协议适配面共享的基础类型，从枢纽头
 *        gateway_protocol_router.h 按域分离，供各协议适配子头
 *        （openai_compat / a2a_handler / mcp_server）单独取用。
 * @details 各适配子头仅依赖 gw_proto_request_handler_t 句柄签名，
 *          无需引入 router 的完整 API 面。拆分后 router.h 扇入由
 *          6 降至 3（G13 load ≤ 5）。
 */

#ifndef AIRY_RT_GATEWAY_PROTO_TYPES_H
#define AIRY_RT_GATEWAY_PROTO_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*gw_proto_request_handler_t)(const char *method, const char *path,
                                          const char *body_json, char **response_json,
                                          void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_GATEWAY_PROTO_TYPES_H */
