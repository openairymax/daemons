/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_crash.h
 * @brief 守护进程致命信号现场落盘（诊断机制，非策略）。
 *
 * 由 daemon_boot 在日志初始化后调用一次；处理器只输出、不恢复，写完即
 * 交由默认动作终止进程。不支持 execinfo 的平台为空实现。
 */

#ifndef AIRY_RT_DAEMON_CRASH_H
#define AIRY_RT_DAEMON_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册致命信号一次性处理器（SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT）。
 *
 * 重复调用幂等。处理器把信号名、帧数与逐帧模块偏移写入 stderr，供离线
 * addr2line 还原调用栈。
 */
void daemon_crash(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CRASH_H */
