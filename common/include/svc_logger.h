/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_logger.h
 * @brief Logging-service compatibility layer (re-export).
 *
 * P0.17 phase 2: the actual definitions moved to
 * commons/utils/logging/svc_logger.h, removing the compile-time
 * reverse dependency atoms->daemons (IRON-6). This file stays as a
 * re-export compat header so daemon sources need no immediate #include
 * path changes.
 *
 * @see commons/utils/logging/svc_logger.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H
#define AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H

/* P0.17 phase 2: include daemon extension error codes (DAEMON_EINIT/
 * ESTATE/EHEALTH etc.). Before the migration the daemons svc_logger.h
 * transitively included the daemons error.h for these codes; after it the
 * commons svc_logger.h no longer does, so include it explicitly here. */
#include "daemon_errors.h"

/* P0.17 phase 2: sources needing daemon-specific declarations (airy_dl_*,
 * airy_sock_*, ...) should #include "daemon_platform_ext.h" directly,
 * not transitively via svc_logger.h. See daemon_platform_ext.h. */


#include "../../../commons/utils/logging/svc_logger.h"

#endif /* AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H */
