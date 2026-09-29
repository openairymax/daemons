// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_d_internal.h
 * @brief gateway_d 进程内跨域契约（main 装配 / cli / acl / mcpclients）。
 *
 * 域切分原则：main.c 只做装配与生命周期分层，CLI 解析、ACL 默认策略、
 * 外部 MCP client 桥各自成域；本头仅声明跨域符号，域内实现不外泄。
 */

#ifndef GATEWAY_D_INTERNAL_H
#define GATEWAY_D_INTERNAL_H

#include "gateway_service.h"

struct gw_mcp_server;

/**
 * @brief 解析命令行参数并写入服务配置。
 *
 * 解析与动作分离：本函数只解析，不做 fork。是否守护化经 @p daemonize
 * 输出，由调用方在**任何线程创建之前**执行 gw_daemonize()。POSIX fork
 * 只复制调用线程；若 fork 时其它线程正持有 malloc/stdio 内部锁，子进程
 * 会继承已锁定状态，在后续清理路径中永久阻塞。
 *
 * @return 0 成功；负值为 AIRY 错误码（含 AIRY_ERROR 宏路径）
 */
int gw_parse_args(int argc, char *argv[], gateway_service_config_t *config,
                  bool *daemonize);

/** @brief 注册外部协议默认 ACL（须在 cupolas PEP 初始化之后调用）。 */
void gw_acl_register_defaults(void);

#ifndef _WIN32

/**
 * @brief 守护化当前进程：fork → 父进程退出 → setsid → umask(022) →
 *        chdir("/") → 标准流重定向 /dev/null。
 *
 * 调用纪律：必须在任何线程创建之前调用（见 gw_parse_args）。
 * @return 0 成功；负值为 AIRY 错误码
 */
int gw_daemonize(void);

/**
 * @brief 读取 AIRY_MCP_CLIENTS env，连接外部 MCP server 并把其工具以
 *        "<client>_<tool>" 前缀注册进网关工具表（失败仅告警不阻塞启动）。
 */
void gw_mcp_clients_setup(struct gw_mcp_server *mcp);

/** @brief 断开全部外部 MCP client 并复位（主退出路径调用）。 */
void gw_mcp_client_cleanup(void);

#endif /* !_WIN32 */

#endif /* GATEWAY_D_INTERNAL_H */
