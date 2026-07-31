// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file daemon_rpc_client.h
 * @brief 轻量级 Unix-socket JSON-RPC 客户端
 *
 * Phase 3 执行体集中化重构：为 gateway_d 内 syscall_router.c 提供
 * 从进程内实现迁移到 daemon IPC 的 thin client。仅暴露同步阻塞接口，
 * 内部完成 socket 连接、JSON-RPC 2.0 请求构造、响应解析与结果提取。
 *
 * 设计目标：
 *   - 与 daemon 端 JSON-RPC 2.0 over Unix socket 协议严格对齐
 *   - 自包含、不依赖 libcurl（区别于 ipc_client.h）
 *   - 调用方负责 AIRY_FREE 返回的 result_json 字符串
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_DAEMON_RPC_CLIENT_H
#define AIRY_RT_DAEMON_RPC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 调用 daemon 上的 JSON-RPC 方法（Unix socket，同步阻塞）
 *
 * @param socket_path  daemon Unix socket 路径（非 NULL）
 * @param method       JSON-RPC 方法名（非 NULL，不含命名空间前缀，如 "write"）
 * @param params_json  params 对象序列化字符串（可为 NULL 表示空 params）
 * @param out_result_json 输出 result 字段的 JSON 序列化字符串（调用方负责 AIRY_FREE）
 * @param timeout_ms   超时（毫秒），0 表示使用默认 30000ms
 * @return AIRY_SUCCESS 成功；其他为错误码
 *
 * @note 失败时 *out_result_json 不会被设置（保持 NULL）。
 *       仅在 POSIX 平台可用；Windows 下返回 AIRY_ERR_NOT_SUPPORTED。
 */
int daemon_rpc_call(const char *socket_path, const char *method,
                    const char *params_json,
                    char **out_result_json, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_RPC_CLIENT_H */
