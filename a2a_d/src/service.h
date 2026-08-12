/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief A2A service internal structure declarations.
 */

#ifndef A2A_SERVICE_INTERNAL_H
#define A2A_SERVICE_INTERNAL_H

#include "a2a_service.h"

#include "platform.h"

#include <a2a_v03_adapter.h>

#include <stddef.h>
#include <stdint.h>

struct a2a_service {
    a2a_v03_context_t *ctx;
    airy_mtx_t lock;
    int initialized;
    size_t max_agents;
    size_t max_tasks;
};

#endif /* A2A_SERVICE_INTERNAL_H */
