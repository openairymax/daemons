/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ipc_service_bus.h
 * @brief IPC service bus (daemons re-export compat header).
 *
 * P0.17 phase 3: the real definitions moved to
 * commons/utils/ipc/include/ipc_service_bus.h, removing the compile-time
 * reverse dependency atoms->daemons (IRON-6). This file stays as a
 * re-export compat header, so daemon sources need no immediate #include
 * changes.
 *
 * @see commons/utils/ipc/include/ipc_service_bus.h (commons authoritative)
 */

#ifndef AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H
#define AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H


#include "../../../commons/utils/ipc/include/ipc_service_bus.h"

#endif /* AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H */
