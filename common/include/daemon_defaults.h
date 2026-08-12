/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file daemon_defaults.h
 * @brief Shared daemon defaults (re-export).
 *
 * P0.17 phase 1: the real definitions moved to commons/include/airy_defaults.h,
 * removing the compile-time reverse dependency atoms->daemons (IRON-6).
 * This file stays as a re-export compat header, so daemon sources
 * (circuit_breaker.c, api_recovery.c, svc_auth.c, llm_d/service.c,
 * tool_d/service.c, etc.) need no immediate #include changes. Later SP
 * tasks can gradually switch daemon references to airy_defaults.h and
 * remove this compat header.
 */

#ifndef AIRY_RT_DAEMON_DEFAULTS_H
#define AIRY_RT_DAEMON_DEFAULTS_H

#include "airy_defaults.h"

#endif /* AIRY_RT_DAEMON_DEFAULTS_H */
