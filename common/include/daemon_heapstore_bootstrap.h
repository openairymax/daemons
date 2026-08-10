// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file daemon_heapstore_bootstrap.h
 * @brief daemon 统一 heapstore 运行时数据存储引导
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * heapstore 是 agentrt 运行时数据存储（KER-05~07：syscall 会话/追踪
 * 持久化，airy_core 链接）。本引导为 daemon 层提供统一初始化入口：
 *   - daemon_heapstore_init()：在 main() 中 airy_log_init() 之后、
 *     socket/service 创建之前调用，初始化 heapstore（幂等）
 *   - daemon_heapstore_cleanup()：main() 退出前调用
 *   - daemon_heapstore_log()：服务访问日志写入（gateway 转发链使用）
 *
 * 与 daemon_cupolas_init 同模式：init 失败记录 FATAL 日志但不 abort，
 * 服务层对不可用存储降级（非致命，保持 daemon 可运行）。
 */

#ifndef AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H
#define AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H

#include "error.h" /* airy_err_t */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 heapstore 运行时数据存储（统一引导）
     *
     * 在 daemon main() 中 airy_log_init() 之后调用（与 daemon_cupolas_init
     * 并列）。存储根目录取 $AIRY_HOME/data/agentrt/heapstore。
     *
     * @param daemon_name daemon 名称（如 "gateway_d"），用于日志标识
     * @return AIRY_SUCCESS 成功；错误码失败（FATAL 日志已记录，服务可降级）
     */
    airy_err_t daemon_heapstore_init(const char *daemon_name);

    /**
     * @brief 清理 heapstore 运行时数据存储
     * 幂等：重复调用安全。
     */
    void daemon_heapstore_cleanup(void);

    /**
     * @brief 写入服务访问日志（gateway 转发链等 daemon 调用）
     *
     * heapstore 不可用（未初始化/写失败）时静默忽略，返回非 0。
     *
     * @param module 模块名（如 "gateway_d"）
     * @param level  日志级别（0=DEBUG,1=INFO,2=WARN,3=ERROR）
     * @param msg    日志消息
     * @param trace_id 追踪 ID（可 NULL）
     * @return 0 成功；非 0 存储不可用/失败
     */
    int daemon_heapstore_log(const char *module, int level, const char *msg,
                             const char *trace_id);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H */
