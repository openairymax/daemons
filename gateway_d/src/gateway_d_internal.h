// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gateway_d_internal.h
 * @brief gateway_d 进程内跨域契约（main 装配 / boot / cli / acl / mcpclients）。
 *
 * 域切分原则：main.c 只做装配骨架与主循环，启停策略（信号/协议装配/SD
 * 公告/健康上报/拆除）、CLI 解析、ACL 默认策略、外部 MCP client 桥各自
 * 成域；本头仅声明跨域符号，域内实现不外泄。
 */

#ifndef GATEWAY_D_INTERNAL_H
#define GATEWAY_D_INTERNAL_H

#include "atomic_compat.h"
#include "gateway_service.h"

struct gw_mcp_server;
struct gw_proto_router;
struct gateway_business_ctx_s;

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

/*
 * boot 域（gw_boot.c）：main.c 装配序经由下列入口驱动策略块。
 */

/** @brief 平台启动：corekern 首启链接 + PEP/IPC/heapstore 平台服务发布；
 *        corekern 失败降级为平台回退（非致命）。须在服务装配之前调用。 */
void gw_plat_boot(void);

/** @brief L2 <ns>.shutdown 停机回调：原子清 @p user_data 指向的存活旗标
 *        （主循环在 1s poll 内退出，与信号路径同语义）。 */
void gw_rpc_stop(void *user_data);

/** @brief 安装进程信号策略（POSIX 信号表 / Win32 控制台句柄）；须在
 *         log_init 之前调用，@p running 为主循环存活旗标。 */
void gw_sig_install(atomic_int *running);

/**
 * @brief 协议路由初始化与 Phase 2 适配器接线（MCP/OpenAI/A2A +
 *        UnifiedProtocol 栈）。
 * @return 0 成功；-1 失败（调用方统一走 gw_teardown 拆除）
 */
int gw_proto_wire(struct gw_proto_router *router,
                  struct gateway_business_ctx_s *biz);

/** @brief 打印实际生效端点摘要并发起 SD 注册（service start 成功后调用）。 */
void gw_sd_announce(gateway_service_t service);

/** @brief 周期健康上报：metrics 门控 + 30s 间隔（主循环每秒调用一次）。 */
void gw_health_tick(gateway_service_t service);

/**
 * @brief 关停拆除级联（进程退出唯一出口）。@p started 为 false 时复刻启动
 *        早退路径（不执行 sd_stop/service_stop）；其余拆除无条件，并收尾
 *        socket 面、IPC/heapstore/cupolas ops 与 log。
 */
void gw_teardown(gateway_service_t service, struct gw_proto_router *router,
                 struct gateway_business_ctx_s *biz, bool started);

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
