// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file airy_event_loop.c
 * @brief Event loop core: platform-independent facade over the backends.
 *
 * 事件循环按职责域拆分（2026-08-27，单文件三平台分支 950 行 → 核心门面 +
 * 三后端 TU）：
 *
 *   本文件        —— 跨后端共享的核心门面（stop 控制、定时器委托）
 *   _epoll/_win/_kqueue
 *                 —— IO 多路复用与回调分发后端（创建/销毁、fd 注册表、
 *                    run 主循环），按平台互斥编译、三选一链接
 *   airy_event_timer.c —— 平台无关定时器管理
 *
 * struct airy_event_loop 的布局由各后端私有定义；本文件仅依赖公共 API 与
 * airy_event_loop_internal.h 导出的内部契约（timers 访问器），所有符号
 * 保持拆分前原名。
 */

#include "airy_event_loop.h"
#include "airy_event_loop_internal.h"

#include "daemon_errors.h"
#include "svc_logger.h"

#ifdef __cplusplus
extern "C" {
#endif

void airy_event_loop_stop(airy_event_loop_t *loop)
{
    if (!loop)
        return;
    airy_event_loop_stop_async(loop);
    AIRY_LOG_DEBUG("Event loop stop requested");
}

uint64_t airy_event_loop_add_timer(airy_event_loop_t *loop, uint64_t interval_ms,
                                   airy_timer_callback_t cb, void *user_data)
{
    if (!loop)
        return 0;
    return airy_timer_add(airy_event_loop_timers(loop), interval_ms, cb, user_data);
}

int airy_event_loop_cancel_timer(airy_event_loop_t *loop, uint64_t timer_id)
{
    if (!loop)
        return AIRY_ERR_INVALID_PARAM;
    return airy_timer_cancel(airy_event_loop_timers(loop), timer_id);
}

#ifdef __cplusplus
}
#endif
