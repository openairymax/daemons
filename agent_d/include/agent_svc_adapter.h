/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_svc_adapter.h
 * @brief Agent service-adapter header.
 *
 * Adapts agent_service_t to the unified AgentRT service framework (airy_svc_t).
 *
 */

#ifndef AIRY_RT_DAEMON_AGENT_SVC_ADAPTER_H
#define AIRY_RT_DAEMON_AGENT_SVC_ADAPTER_H

#include "agent_service.h"
#include "svc_common.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

AIRY_API int agent_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config);

AIRY_API int agent_service_adapter_wrap(airy_svc_t *out_service, agent_service_t *agent_svc,
                                        const airy_svc_config_t *config);

AIRY_API agent_service_t *agent_service_adapter_get_original(airy_svc_t service);

AIRY_API int agent_service_adapter_init(airy_svc_t service);
AIRY_API int agent_service_adapter_start(airy_svc_t service);
AIRY_API int agent_service_adapter_stop(airy_svc_t service, bool force);
AIRY_API void agent_service_adapter_destroy(airy_svc_t service);
AIRY_API int agent_service_adapter_healthcheck(airy_svc_t service);

AIRY_API const airy_svc_interface_t *agent_service_adapter_get_interface(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_SVC_ADAPTER_H */
