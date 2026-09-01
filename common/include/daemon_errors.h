/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_errors.h
 * @brief Daemon-module extension error codes.
 *
 * P0.17 phase 2: extracted the daemon-module extension codes from
 * daemons/common/include/error.h into this standalone header, so daemon
 * sources can include it directly to get the daemon extension codes,
 * without depending on the daemons error.h (due to -I path ordering,
 * #include "error.h" resolves to the commons error.h first).
 *
 * Code range: -910 to -949 (the original -900 range collided with
 * commons' AIRY_ERR_PROTOCOL; moved to the free -910 range in G2.2).
 *
 * @see commons/utils/error/error.h  commons authoritative codes
 * @see daemons/common/include/error.h       daemons compat layer (includes this)
 */

#ifndef AIRY_RT_DAEMON_ERRORS_H
#define AIRY_RT_DAEMON_ERRORS_H


/*
 * Code range: -910 to -949
 *
 * History: originally -900 (migrated from -600 in G2.2), but commons later
 * defined AIRY_ERR_PROTOCOL/AIRY_ERR_CHECKSUM at -900/-901, causing a
 * conflict. Migrated to the -910 range in P0.17 phase 2.
 */
#ifndef AIRY_ERR_DAEMON_BASE
#define AIRY_ERR_DAEMON_BASE (-910)
#endif
#ifndef AIRY_ERR_DAEMON_AUTH_FAIL
#define AIRY_ERR_DAEMON_AUTH_FAIL (AIRY_ERR_DAEMON_BASE + 0x01)
#endif
#ifndef AIRY_ERR_DAEMON_CONFIG_INVALID
#define AIRY_ERR_DAEMON_CONFIG_INVALID (AIRY_ERR_DAEMON_BASE + 0x02)
#endif
#ifndef AIRY_ERR_DAEMON_INIT_FAILED
#define AIRY_ERR_DAEMON_INIT_FAILED (AIRY_ERR_DAEMON_BASE + 0x03)
#endif
#ifndef AIRY_ERR_DAEMON_ALREADY_INIT
#define AIRY_ERR_DAEMON_ALREADY_INIT (AIRY_ERR_DAEMON_BASE + 0x04)
#endif


#ifndef AIRY_ERR_ALREADY_INIT
#define AIRY_ERR_ALREADY_INIT AIRY_ERR_DAEMON_ALREADY_INIT
#endif


/*
 * P0.17 phase 2: aliases unifying the legacy DAEMON_E* codes used in
 * daemon sources, mapping them to commons authoritative codes or daemon
 * extension codes.
 */
#ifndef DAEMON_EINIT
#define DAEMON_EINIT AIRY_ERR_DAEMON_INIT_FAILED
#endif
#ifndef DAEMON_ESTATE
#define DAEMON_ESTATE AIRY_ERR_SVC_NOT_READY
#endif
#ifndef DAEMON_EHEALTH
#define DAEMON_EHEALTH AIRY_ERR_SVC_HEALTH
#endif
#ifndef DAEMON_EFAIL
#define DAEMON_EFAIL AIRY_ERR_UNKNOWN
#endif
#ifndef DAEMON_EDEPEND
#define DAEMON_EDEPEND AIRY_ERR_SVC_DEPENDENCY
#endif

#endif /* AIRY_RT_DAEMON_ERRORS_H */
