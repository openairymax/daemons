// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file mem_svc_adapter.h
 * @brief Memory 服务适配器头文件
 *
 * 将 mem_service_t 适配到统一 AgentRT 服务管理框架（airy_svc_t）。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_DAEMON_MEM_SVC_ADAPTER_H
#define AIRY_RT_DAEMON_MEM_SVC_ADAPTER_H

#include "mem_service.h"
#include "svc_common.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

AIRY_API int mem_service_adapter_create(airy_svc_t *out_service,
                                          const airy_svc_config_t *config);

AIRY_API int mem_service_adapter_wrap(airy_svc_t *out_service,
                                        mem_service_t *mem_svc,
                                        const airy_svc_config_t *config);

AIRY_API mem_service_t *mem_service_adapter_get_original(airy_svc_t service);

AIRY_API int mem_service_adapter_init(airy_svc_t service);
AIRY_API int mem_service_adapter_start(airy_svc_t service);
AIRY_API int mem_service_adapter_stop(airy_svc_t service, bool force);
AIRY_API void mem_service_adapter_destroy(airy_svc_t service);
AIRY_API int mem_service_adapter_healthcheck(airy_svc_t service);

AIRY_API const airy_svc_interface_t *mem_service_adapter_get_interface(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MEM_SVC_ADAPTER_H */
