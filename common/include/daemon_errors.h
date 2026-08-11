/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_errors.h
 * @brief Daemon 模块扩展错误码定义
 *
 * P0.17 阶段 2：从 daemons/common/include/error.h 中提取 daemon 模块
 * 扩展错误码至独立头文件，使 daemons 内部源文件可直接包含此文件获取
 * daemon 扩展错误码，而不依赖 daemons 版 error.h（因 -I 路径顺序，
 * #include "error.h" 会优先找到 commons 版 error.h）。
 *
 * 错误码段：-910 到 -949（原 -900 段与 commons 的 AIRY_ERR_PROTOCOL
 * 冲突，G2.2 迁移至 -910 空闲段）。
 *
 * @see commons/utils/error/include/error.h  commons 权威错误码定义
 * @see daemons/common/include/error.h       daemons 兼容层（包含本文件）
 */

#ifndef AIRY_RT_DAEMON_ERRORS_H
#define AIRY_RT_DAEMON_ERRORS_H


/*
 * 错误码段：-910 到 -949
 *
 * 历史：原使用 -900 段（G2.2 迁移自 -600），但 commons 后续在 -900/-901
 * 定义了 AIRY_ERR_PROTOCOL/AIRY_ERR_CHECKSUM，导致冲突。
 * P0.17 阶段 2 迁移至 -910 段以消除冲突。
 */
#ifndef AIRY_ERR_DAEMON_BASE
#define AIRY_ERR_DAEMON_BASE (-910)
#endif
#ifndef AIRY_ERR_DAEMON_AUTH_FAIL
#define AIRY_ERR_DAEMON_AUTH_FAIL (AIRY_ERR_DAEMON_BASE + 0x01)
#endif
#ifndef AIRY_ERR_DAEMON_CONFIG_INVALID
#define AIRY_ERR_DAEMON_CONFIG_INVALID (AIRY_ERR_DAEMON_BASE + 0x02)
#endif
#ifndef AIRY_ERR_DAEMON_INIT_FAILED
#define AIRY_ERR_DAEMON_INIT_FAILED (AIRY_ERR_DAEMON_BASE + 0x03)
#endif
#ifndef AIRY_ERR_DAEMON_ALREADY_INIT
#define AIRY_ERR_DAEMON_ALREADY_INIT (AIRY_ERR_DAEMON_BASE + 0x04)
#endif


#ifndef AIRY_ERR_ALREADY_INIT
#define AIRY_ERR_ALREADY_INIT AIRY_ERR_DAEMON_ALREADY_INIT
#endif


/*
 * P0.17 阶段 2：以下别名统一 daemons 内部源文件中使用的旧式 DAEMON_E* 错误码
 * 名称，映射到 commons 权威错误码或 daemon 扩展错误码。
 */
#ifndef DAEMON_EINIT
#define DAEMON_EINIT AIRY_ERR_DAEMON_INIT_FAILED
#endif
#ifndef DAEMON_ESTATE
#define DAEMON_ESTATE AIRY_ERR_SVC_NOT_READY
#endif
#ifndef DAEMON_EHEALTH
#define DAEMON_EHEALTH AIRY_ERR_SVC_HEALTH
#endif
#ifndef DAEMON_EFAIL
#define DAEMON_EFAIL AIRY_ERR_UNKNOWN
#endif
#ifndef DAEMON_EDEPEND
#define DAEMON_EDEPEND AIRY_ERR_SVC_DEPENDENCY
#endif

#endif /* AIRY_RT_DAEMON_ERRORS_H */
