// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_cfg_file.h
 * @brief Daemon 配置文件读取与端点段解析（机制唯一实现）。
 *
 * 0.1.19 t44：config_load/ep_load 家族七副本消解的机制载体。文件读取
 * 样板（fopen/fread/限长/JSON 解析/释放）与 daemon 段端点三元组
 * （socket_path/tcp_port/use_tcp）解析统一在此；守护进程仅保留 env
 * 覆盖与专属字段提取等策略件（机制与策略分离）。
 */

#ifndef AIRY_RT_DAEMON_CFG_FILE_H
#define AIRY_RT_DAEMON_CFG_FILE_H

#include "error.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 端点配置三元组：机制解析产出。socket_path 为 AIRY_STRDUP 拥有串，
 * 调用者 AIRY_FREE；tcp_port 已过范围校验（>0 且 <=65535）。 */
typedef struct {
    char *socket_path;
    int use_tcp;
    int tcp_port;
} daemon_ep_cfg_t;

/* 配置文件解析回调：root 为整文档根对象（借用，回调内勿释放）。 */
typedef void (*daemon_cfg_fn)(cJSON *root, void *ud);

/**
 * @brief 端点三元组缺省基线（平台分支 socket + TCP 口基线）。
 * @return AIRY_SUCCESS；ep 为空返回 AIRY_ERR_INVALID_PARAM。
 */
int daemon_ep_def(const char *sock_unix, const char *sock_win, int tcp_port,
                  daemon_ep_cfg_t *ep);

/**
 * @brief 读取 config 文件并以整文档根对象派发回调（读取样板机制）。
 *
 * config_path 为空或不可读时静默返回 AIRY_SUCCESS（缺省即终态，与
 * 既有守护进程行为一致）；单文件限长 1 MiB；JSON 解析失败不派发。
 */
int daemon_cfg_read(const char *config_path, daemon_cfg_fn fn, void *ud);

/**
 * @brief 从 "daemon" 对象解析端点三元组（覆盖式，未出现的字段保持现值）。
 *
 * daemon_cfg 为空时无操作。tcp_port 严格校验（>0 且 <=65535），非法
 * 值回落现值；use_tcp 仅在合法端口出现时置位。
 */
void daemon_ep_parse(const cJSON *daemon_cfg, daemon_ep_cfg_t *ep);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CFG_FILE_H */
