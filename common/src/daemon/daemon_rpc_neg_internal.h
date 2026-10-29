/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_rpc_neg_internal.h
 * @brief Internal contract of the daemon RPC reachability negative cache
 *        (not public API).
 *
 * Split out of daemon_rpc_client.c so that the cache keeps its own table,
 * lock and lifecycle in one file, apart from the request orchestration
 * that only queries and updates it.
 *
 * This header is for the daemon_rpc_* sources only; it must not be used
 * by other modules.
 *
 * @see agentrt/daemons/common/include/daemon_rpc_client.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_DAEMON_RPC_NEG_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_DAEMON_RPC_NEG_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 目标是否仍在退避窗口内（「不可达事实」未过期）。 */
int rpc_neg_blocked(const char *target);

/* 记录一次连接失败：仅对曾连通过的目标升级档位并顺延窗口。 */
void rpc_neg_fail(const char *target);

/* 记录一次连接成功：登记目标并清零退避。 */
void rpc_neg_ok(const char *target);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_DAEMON_RPC_NEG_INTERNAL_H */
