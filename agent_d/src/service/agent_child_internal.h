/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_child_internal.h
 * @brief Agent 子进程面：spec 解析、运行日志保留与跨平台传输原语。
 *
 *        与 agent_service_internal.h 分开是为守住 G13 扇入上限：服务骨架
 *        （service/stats）只看生命周期，传输（child_posix/child_win）、
 *        调用与派生（invoke/spawn）只经本头协作。本头重新导出服务内部头，
 *        因此子进程实现依旧拿到统一的预置依赖，不需要各自拼接。
 */

#ifndef AIRY_RT_DAEMON_AGENT_D_AGENT_CHILD_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_D_AGENT_CHILD_INTERNAL_H

#include "agent_service_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spec parsing and agent log retention shared by both transports
 * (service_child.c). */
void agent_spec_language(const char *spec, char *buf, size_t size);
void agent_spec_rust_bin(const char *spec, char *out_path, size_t cap);
void agent_child_log_path(const char *agent_id, char *out_path, size_t cap);
void agent_logs_prune(void);

/* Child process communication (service_child_posix.c / service_child_win.c) -
 * implemented on POSIX and Windows alike (0.1.21 P0-1); pid is carried as
 * long, which holds pid_t and Windows DWORD. The non-cancel entry point is a
 * thin wrapper in service_child.c so the two transports do not duplicate it. */
int agent_read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_s);
int agent_read_line_timeout_ex(int fd, char *buf, size_t buf_size, int timeout_s,
                               airy_cancel_token_t *token);
int agent_spawn_child(const char *spec, const char *agent_id, long *out_pid, int *out_stdin,
                      int *out_stdout);

/* 逐字节读行循环的公共状态机（service_child.c）：POSIX 与 Windows 传输
 * 共用，防止两平台尾段各写一份后漂移。返回码即调用方的返回码，只有
 * AGENT_LINE_MORE 表示“这一字节不足成行，继续读”。
 * 返回：0=整行就绪（换行已截断）；1=缓冲写满（已截断成串）；-1=EOF/对端
 * 关闭；AGENT_LINE_MORE=继续读取。 */
#define AGENT_LINE_MORE 3
int agent_line_step(char *buf, size_t buf_size, size_t *pos, int eof);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_D_AGENT_CHILD_INTERNAL_H */
