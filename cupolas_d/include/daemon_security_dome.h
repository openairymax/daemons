/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_security_dome.h
 * @brief 安全穹顶策略面：cupolas 穹顶统一引导 + vault provider 实现。
 *
 * 机制/策略分界的策略侧：svc_common 只提供安全机制核
 * （daemon_security_*），不链接任何具体凭据后端；穹顶装配
 * （cupolas_init / vault / entitlements / netsec）与 cupolas vault
 * provider 由本单元实现，并经 daemon_security_config_t.vault_provider
 * 单向注入机制核。穹顶由 airy_security_dome 静态库导出。
 *
 * 调用契约：
 *   - 在 main() 中 airy_log_init() 之后、socket/服务创建之前调用
 *     daemon_dome_init()
 *   - 在 main() 退出前调用 daemon_dome_cleanup()
 *   - init 失败时资源已释放，无需再调用 cleanup
 *
 * 安全语义：
 *   - cupolas_init 使用默认配置（NULL config_path），启用
 *     permission_engine + sanitizer + audit_logger 子模块
 *   - 失败时打印 ERROR 日志但不中止进程：daemon 可降级运行，
 *     各服务层的 fail-closed 逻辑会阻断危险操作
 *   - 重复调用 init 幂等（穹顶内部有守卫）
 */

#ifndef AIRY_RT_DAEMON_SECURITY_DOME_H
#define AIRY_RT_DAEMON_SECURITY_DOME_H

#include "error.h" /* airy_err_t */
#include "cupolas_vault.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化安全穹顶（统一引导，完整 PDP 形态）。
 *
 * @param daemon_name 守护进程名（如 "tool_d"、"llm_d"），用于审计日志
 * @return AIRY_SUCCESS 成功；失败返回错误码（已记录日志）
 *
 * @ownership daemon_name: BORROW（调用方保留所有权）
 */
airy_err_t daemon_dome_init(const char *daemon_name);

/**
 * @brief 以 PEP（策略执行点）最小 guard 形态初始化安全穹顶（0.1.9 M2 §3.2）。
 *
 * 与 daemon_dome_init() 相同初始化本地必需的 sanitizer/workbench/audit
 * 与 daemon_security（PDP 不可达时的 fallback ACL），但**跳过本地
 * vault / entitlements / network_security**——这些由 PDP（cupolas_d）
 * 集中持有，PEP 经 RPC 转发访问（gateway cap 注册表 GW_CAP_KIND_FWD）。
 *
 * @param daemon_name 守护进程名（如 "gateway_d"），用于审计日志
 * @return AIRY_SUCCESS 成功；失败返回错误码（已记录日志）
 */
airy_err_t daemon_dome_init_pep(const char *daemon_name);

/**
 * @brief 关闭安全穹顶。
 *
 * 在 daemon main() 退出前调用：刷写审计日志并释放资源。幂等。
 */
void daemon_dome_cleanup(void);

/**
 * @brief 穹顶自持的 cupolas vault 句柄（provider 打开成功后有效）。
 *
 * 供 cupolas_d 的 RPC 面执行 vault 删除/列表/轮换等 provider 接口之外
 * 的操作。未启用 vault 或 provider 未打开时返回 NULL。
 *
 * @ownership 返回句柄归穹顶 provider 所有，调用方 BORROW
 */
cupolas_vault_t *cupolas_dome_vault(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_SECURITY_DOME_H */
