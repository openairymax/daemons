// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_loop_internal.h
 * @brief Internal contract between the event loop core and platform backends.
 *
 * airy_event_loop.c 自 2026-08-27 起按职责域拆分：
 *
 *   airy_event_loop.c         事件循环核心（平台无关门面：stop / 定时器委托）
 *   airy_event_loop_epoll.c   Linux epoll 后端（IO 多路复用 + 回调分发）
 *   airy_event_loop_win.c     Windows WSAEventSelect 后端
 *   airy_event_loop_kqueue.c  macOS/BSD kqueue 后端
 *   airy_event_timer.c        平台无关定时器管理（更早拆出）
 *
 * struct airy_event_loop 的布局由各后端私有定义（字段随多路复用机制而异，
 * 且同一平台仅有一个后端参与链接），核心层不依赖其布局；跨编译单元共享的
 * 内部符号统一经本头文件以原名 extern 导出。
 *
 * NOT part of the public API.
 */

#ifndef AIRY_EVENT_LOOP_INTERNAL_H
#define AIRY_EVENT_LOOP_INTERNAL_H

#include "airy_event_loop.h"
#include "airy_event_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Accessor to the timer subsystem owned by a backend loop instance.
 *
 * 由当前参与链接的平台后端提供（epoll / win / kqueue 三选一）；事件循环
 * 核心的定时器委托入口（airy_event_loop_add_timer / cancel_timer）经此
 * 触达 timers 域，避免核心层依赖任何后端的结构体布局。
 */
airy_timer_state_t *airy_event_loop_timers(airy_event_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_EVENT_LOOP_INTERNAL_H */
