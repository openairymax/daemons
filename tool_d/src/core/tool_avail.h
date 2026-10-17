/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_avail.h
 * @brief 工具可用性视图（0.1.19 D8）：把"能力存在"与"依赖可用"绑定。
 *
 * "能力存在 ≠ 能力可用"：注册表只回答"声明了哪些工具"，不能回答"该工具
 * 此刻是否真的能调用"。maths_* 依赖 maths_d，后者掉线时若仍暴露给 agent，
 * 就会出现"暴露成功 → 调用三连败 → 报服务不可达"的时序矛盾。
 *
 * 本件为该问题提供唯一机制实现——按声明的依赖 socket 发起幂等可达探测
 * （AF_UNIX connect），并向注册表暴露一个"该工具此刻是否可暴露"的谓词。
 * 语义三条：
 *   1. 无依赖声明 ⇒ 可用（能力不依赖外部 daemon）；
 *   2. 有依赖但尚未探测（unknown）⇒ 视为可用（fail-open，不隐藏）；
 *   3. 探测结论为不可达 ⇒ 不可用（列举侧摘除），恢复后自动重挂。
 *
 * 依赖目标直接携带 socket 文件名（如 "maths.sock"），不做 daemon 名 →
 * socket 名派生：实测 X_d → <cname>.sock 存在例外（a2d_d → a2a.sock），
 * 派生不可靠。
 *
 * 机制与策略分离：探测周期由策略层（svc.c）注入；本件只负责"声明一次、
 * 探测一次、到账一次"。
 */

#ifndef TOOL_AVAIL_H
#define TOOL_AVAIL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_avail tool_avail_t;

/* 建立可用性视图。内存不足返回 NULL。 */
tool_avail_t *tool_avail_new(void);

void tool_avail_free(tool_avail_t *ta);

/* 声明一条"tool_id 依赖 sock_name 可达"的链接。同一 sock_name 只登记
 * 一次（多工具共享同一依赖）。返回 0 成功，负值失败（容量满/参数非法）。 */
int tool_avail_dep(tool_avail_t *ta, const char *tool_id, const char *sock_name);

/* 对全部已知依赖 socket 发起一次幂等可达探测并刷新状态。运行在 daemon
 * 线程池 worker 上（探测量级为依赖数量级，单发 connect 为本地微秒级）。 */
void tool_avail_poll(tool_avail_t *ta);

/* 该工具此刻是否可暴露：返回非 0 表示可暴露。无依赖声明、依赖未知、
 * 依赖可达三种情形均返回 1；仅"已知不可达"返回 0。 */
int tool_avail_ok(tool_avail_t *ta, const char *tool_id);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_AVAIL_H */
