// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sup_net.h
 * @brief 端点机制件 net.c 的接口面（V13.5 零依赖硬约束下的模块内 SSoT）。
 *
 * 机制/策略分离：本头只暴露「端点字面量 → 可连接 fd / 可达判据」的机制
 * 契约，不泄漏进程记录类型——策略层（ctrl.c 控制口、proc.c 假死探测）
 * 只持 fd 与端点字面量，故 sup_probe_sock() 直接收端点串，不依赖
 * sup_proc_t。这样 net.c 成为真正自持的机制件：既不解 supervisor_d.h 的
 * 聚合面，也不反向依赖策略层类型。
 *
 * 拆面依据（G13 load ≤ 5）：supervisor_d.h 为六件共用的聚合头，net.c 若
 * 直接 include 会将 src 面扇入推升至 6；机制件自持接口头后，聚合头扇入
 * 回落至 5，机制头扇入为 2（net.c 与本头），双双达标。
 */

#ifndef AIRY_SUP_NET_H
#define AIRY_SUP_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 假死探测单次 sock 可达性超时（非阻塞 connect + select，见 net.c） */
#define SUP_PROBE_TIMEOUT_MS 500

/* ---- 端点字面量裁剪：host:port → 两段（跨平台，可单测） ---- */
int sup_ep_split(const char *ep, char *host, size_t hsz, char *port, size_t psz);

/* ---- sock 可达性判据：端点串空返回 -1（调用方须走进程存活腿） ---- */
int sup_probe_sock(const char *sock);

#ifdef _WIN32
/* Windows 专有：TCP 回环装配（POSIX 走 UDS，不经此路） */
int sup_tcp_conn(const char *ep, int timeout_ms);
int sup_tcp_bind(const char *ep);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIRY_SUP_NET_H */
