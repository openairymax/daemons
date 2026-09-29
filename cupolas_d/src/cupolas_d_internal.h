// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_d_internal.h
 * @brief Internal shared declarations of the cupolas_d daemon translation
 *        units (svc.c / cupolas_rpc_*.c / service.c).
 * @details RPC method entry points (svc_on_*_cupolas_d) and the daemon
 *          lifecycle hooks are declared in the generated svc_cupolas_d.h
 *          (L3 SSoT); this header only carries the daemon-wide cupolas
 *          service handle and the dynamic policy engine (PDP) handle.
 *          For use only by the cupolas_d daemon translation units.
 */

#ifndef AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H
#define AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H

#include "cupolas_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-wide cupolas service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern cupolas_service_t *g_service;

/* Dynamic policy engine handle (PDP, owned/lifecycled in svc.c,
 * referenced by the policy RPC handlers). */
extern struct dpolicy_engine_s *g_dpolicy;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H */
