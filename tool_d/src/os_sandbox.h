/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @file os_sandbox.h */


#ifndef AIRY_RT_TOOL_OS_SANDBOX_H
#define AIRY_RT_TOOL_OS_SANDBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sandbox modes (mirroring the Landlock filesystem sandbox of Codex
 * linux-sandbox):
 * - OFF: no OS-level isolation (only existing timeout/output truncation),
 *   for platforms without a sandbox-capable kernel
 * - WORKSPACE: global read-only + workspace writable (default; shell
 *   commands can read the system and the workspace, but any write/delete
 *   to system dirs is denied by Landlock)
 * - STRICT: only system base paths + workspace readable/executable,
 *   workspace writable; network disabled by default */
typedef enum {
    OS_SANDBOX_MODE_OFF = 0,
    OS_SANDBOX_MODE_WORKSPACE = 1,
    OS_SANDBOX_MODE_STRICT = 2,
} os_sandbox_mode_t;

typedef struct {
    os_sandbox_mode_t mode;
    char workspace[1024];
    int net_access;
    uint64_t mem_limit_mb;
    uint32_t nofile_limit;
    uint32_t nproc_limit;
    uint32_t cpu_limit_sec;
} os_sandbox_cfg_t;


int os_sandbox_landlock_available(void);

/* Build a default config from environment variables:
 *   AIRY_TOOL_SANDBOX_MODE=off|workspace|strict (default workspace)
 *   AIRY_TOOL_SANDBOX_WORKSPACE=<absolute path> (default getcwd)
 *   AIRY_TOOL_SANDBOX_NET=0|1                   (overrides net_access) */
void os_sandbox_cfg_from_env(os_sandbox_cfg_t *cfg);

/* Call after fork, before exec: apply rlimit + seccomp + Landlock.
 * Returns 0 on success; negative on failure (fail-closed: the caller
 * should refuse to execute).
 * Note: mode==STRICT returns failure when Landlock is unavailable (strict
 * mode does not allow degradation); mode==WORKSPACE degrades gracefully
 * when Landlock is unavailable (returns 0, logs a warning). */
int os_sandbox_apply(const os_sandbox_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_OS_SANDBOX_H */
