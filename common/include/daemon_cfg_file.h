// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_cfg_file.h
 * @brief Daemon 配置文件读取与端点装配（机制唯一实现）。
 *
 * 0.1.19 t44/t54：config_load/ep_load 家族七副本与 svc_endpoint 六行体
 * 全仓消解的机制载体。文件读取样板（fopen/fread/限长/JSON 解析/释放）、
 * daemon 段端点三元组（socket_path/tcp_port/use_tcp）解析与端点五元组
 * 装配（daemon_ep_fill/daemon_ep_base）统一在此；守护进程仅保留 env
 * 覆盖与专属键提取等策略件（机制与策略分离）。
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

/* 策略键回调：收整文档根对象（借用）。daemon 段容量键与 compress/
 * think 等专属段提取均为策略件，由各户自持；机制件只负责端点三元组
 * 与文件读取样板。 */
typedef void (*daemon_keys_fn)(const cJSON *root, void *user);

/* 服务端点五元组：svc_endpoint 策略件经 daemon_ep_fill/daemon_ep_base
 * 装配产出，daemon_boot 套接字创建与事件驱动装配按字段序消费。 */
typedef struct {
    int use_tcp;
    const char *tcp_host;
    int tcp_port;
    const char *sock_unix;
    const char *sock_win;
} daemon_endpoint_t;

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

/**
 * @brief 端点配置装载（def 基线 + 文件覆盖 + 策略键派发，七副本消解）。
 *
 * 先建立基线（sock_unix/sock_win 平台分支 + tcp_port），再读 config
 * 文件覆盖式解析端点三元组，并将整文档根对象派发给 keys 策略键回调
 * （可空）。优先级：内置基线 < config 文件；env 覆盖属策略件，由
 * 调用者自行处理。ep 空参数返回 AIRY_ERR_INVALID_PARAM，其余缺省即
 * 终态。
 */
int daemon_ep_load(daemon_ep_cfg_t *ep, const char *config_path,
                   const char *sock_unix, const char *sock_win, int tcp_port,
                   daemon_keys_fn keys, void *user);

/**
 * @brief 释放端点配置（socket_path 归还并整体清零，幂等可重入）。
 */
void daemon_ep_free(daemon_ep_cfg_t *ep);

/**
 * @brief 端点配置 → 服务端点五元组装配（svc_endpoint 唯一机制体）。
 *
 * use_tcp 融合 cmdline --tcp（只升不降）；tcp_host 固定环回；
 * socket 双路取自配置解析产出。
 */
void daemon_ep_fill(daemon_endpoint_t *out, const daemon_ep_cfg_t *ep,
                    int cmdline_tcp);

/**
 * @brief 基线直填（无 config 户）：生成头常量端点 → 服务端点五元组。
 */
void daemon_ep_base(daemon_endpoint_t *out, int cmdline_tcp,
                    const char *sock_unix, const char *sock_win, int tcp_port);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CFG_FILE_H */
