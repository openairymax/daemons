/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_common_internal.h
 * @brief svc_common 静态库内部共享定义（非公共 API）
 *
 * P1.x 模块化拆分：svc_common.c 拆分为 svc_registry.c / svc_config.c /
 * svc_monitor.c / svc_client.c 后，以下定义被多个源文件共享：
 *
 * - airy_svc_internal_t：服务实例内部结构。svc_common.c（生命周期/内部注册表）
 *   与 svc_client.c（本地直连客户端读取 iface/user_data）均需访问其字段，
 *   故移至内部头而非留在单一 .c 文件。
 * - MAX_SERVICE_NAME_LEN / MAX_SERVICE_VERSION_LEN：结构体内定长数组依赖，
 *   svc_common.c 的 airy_svc_create() 亦使用。
 * - monitor_shutdown()：monitor 域（svc_monitor.c）持有监控全局状态，由
 *   svc_common.c 的 airy_svc_common_cleanup() 调用以销毁监控互斥锁。
 *
 * 本头仅供 svc_common 静态库内部源文件包含，不得被 daemons 其他模块使用。
 *
 * @see agentrt/daemons/common/src/svc_common.c
 * @see agentrt/daemons/common/src/svc_monitor.c
 * @see agentrt/daemons/common/src/svc_client.c
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H

#include "svc_common.h"

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SERVICE_NAME_LEN 64
#define MAX_SERVICE_VERSION_LEN 32

/**
 * @brief Service instance internal structure
 */
typedef struct airy_svc_internal {

    char name[MAX_SERVICE_NAME_LEN];
    char version[MAX_SERVICE_VERSION_LEN];

    airy_svc_state_t state;
    airy_mtx_t state_mutex;

    airy_svc_config_t config;
    uint32_t capabilities;

    airy_svc_stats_t stats;
    airy_mtx_t stats_mutex;

    airy_svc_interface_t iface;

    uint64_t last_healthcheck_time;
    int healthcheck_failures;

    void *user_data;

    void *thread_pool;
    pthread_t *threads;
    size_t thread_count;

    struct airy_svc_internal *next;
} airy_svc_internal_t;

/**
 * @brief Shut down the service monitor subsystem.
 * @note 实现位于 svc_monitor.c（g_monitor 状态归 monitor 域私有），
 *       由 svc_common.c 的 airy_svc_common_cleanup() 在进程退出路径调用。
 */
void monitor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H */
