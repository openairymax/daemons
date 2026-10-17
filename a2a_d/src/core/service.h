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
#include "unified_protocol.h"

#include <stddef.h>
#include <stdint.h>

struct a2a_service {
    const protocol_adapter_t *adapter;
    void *context;
    airy_mtx_t lock;
    int initialized;
};

#endif /* A2A_SERVICE_INTERNAL_H */
