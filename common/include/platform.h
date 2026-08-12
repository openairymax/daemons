/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file platform.h
 * @brief Platform-abstraction compat layer (daemon-specific) - re-export.
 *
 * P0.17 phase 2: daemon-specific declarations moved to daemon_platform_ext.h.
 * This file is only a backward-compatible re-export layer.
 *
 * Note: due to -I path ordering (commons/platform/include precedes
 * daemons/common/include), #include "platform.h" in daemon sources usually
 * resolves to the commons platform.h. Sources needing daemon-specific
 * declarations should #include "daemon_platform_ext.h" directly.
 *
 * @see agentrt/commons/platform/include/platform.h  (commons authoritative)
 * @see daemon_platform_ext.h                         (daemon-specific extension)
 */

#ifndef AIRY_RT_DAEMON_COMMON_PLATFORM_H
#define AIRY_RT_DAEMON_COMMON_PLATFORM_H

#include "daemon_platform_ext.h"

#endif /* AIRY_RT_DAEMON_COMMON_PLATFORM_H */
