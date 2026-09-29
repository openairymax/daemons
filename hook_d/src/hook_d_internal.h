/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file hook_d_internal.h
 * @brief hook_d 守护进程各编译单元共享的内部声明
 *        （svc.c / hook_rpc.c）。
 * @details RPC 方法入口（m_*）与生命周期钩子声明在
 *          生成的 svc_hook_d.h（L3 SSoT）；本头仅承载 daemon 私有的
 *          注册表状态量与会话级上下文注入通道（P1-5）的存储与访问
 *          原语。仅限 hook_d 守护进程编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_HOOK_D_INTERNAL_H
#define AIRY_RT_DAEMON_HOOK_D_INTERNAL_H

#include "platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIRY_HOOK_MAX_SESSIONS 32
#define AIRY_HOOK_CTX_LEN 4096

typedef struct {
    char session_id[64];
    bool active;
    uint64_t started_ns;
    char decision[16];
    size_t hook_count;
    char context[AIRY_HOOK_CTX_LEN]; /* 注入的会话上下文（session.start 的 input） */
    size_t context_len;
} hook_session_entry_t;

/* Hook 系统引导标志与启动时刻（svc.c 拥有，RPC handler 只读） */
extern int g_registry_initialized;
extern uint64_t g_start_time;

/* P1-5 会话级上下文注入通道存储（svc.c 初始化/销毁锁，
 * handler 经 session_find / session_upsert 访问） */
extern hook_session_entry_t g_hook_sessions[AIRY_HOOK_MAX_SESSIONS];
extern airy_mtx_t g_hook_sessions_lock;

/* 会话表访问原语（svc.c 实现；调用方负责持锁） */
hook_session_entry_t *hook_session_find(const char *session_id);
hook_session_entry_t *hook_session_upsert(const char *session_id);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_HOOK_D_INTERNAL_H */
