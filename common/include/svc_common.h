/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_common.h
 * @brief Common service definitions (daemons re-export compat header).
 *
 * P0.17 phase 3: the real definitions moved to
 * commons/utils/ipc/include/svc_common.h, removing the compile-time reverse
 * dependency atoms->daemons (IRON-6). This file stays as a re-export
 * compat header, so daemon sources need no immediate #include changes.
 *
 * Additionally includes daemon_errors.h to provide the daemons extension
 * error codes (DAEMON_E* aliases), which the commons svc_common.h does not
 * include.
 *
 * @see commons/utils/ipc/include/svc_common.h (commons authoritative)
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_COMMON_H
#define AIRY_RT_DAEMON_COMMON_SVC_COMMON_H


#include "daemon_errors.h"


#include "../../../commons/utils/ipc/include/svc_common.h"

#endif /* AIRY_RT_DAEMON_COMMON_SVC_COMMON_H */
