/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file llm_svc_adapter.h
 * @brief LLM服务适配器头文件
 *
 * 提供LLM服务与AgentRT统一服务管理框架的适配接口。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_DAEMON_LLM_SVC_ADAPTER_H
#define AIRY_RT_DAEMON_LLM_SVC_ADAPTER_H

#include "llm_service.h"
#include "svc_common.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

AIRY_API airy_err_t llm_service_adapter_create(airy_svc_t *out_service,
                                                       const airy_svc_config_t *config);

AIRY_API airy_err_t llm_service_adapter_wrap(airy_svc_t *out_service,
                                                     llm_service_t llm_svc,
                                                     const airy_svc_config_t *config);

AIRY_API llm_service_t llm_service_adapter_get_original(airy_svc_t service);

AIRY_API airy_err_t llm_service_adapter_init(airy_svc_t service);
AIRY_API airy_err_t llm_service_adapter_start(airy_svc_t service);
AIRY_API airy_err_t llm_service_adapter_stop(airy_svc_t service, bool force);
AIRY_API void llm_service_adapter_destroy(airy_svc_t service);
AIRY_API airy_err_t llm_service_adapter_healthcheck(airy_svc_t service);

AIRY_API const airy_svc_interface_t *llm_service_adapter_get_interface(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_LLM_SVC_ADAPTER_H */
