/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @file os_sandbox.h */


#ifndef AIRY_RT_TOOL_OS_SANDBOX_H
#define AIRY_RT_TOOL_OS_SANDBOX_H

#include <stddef.h>
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

/* Resolve a tool-supplied path against the workspace sandbox and write the
 * canonical result into `resolved`. Unlike os_sandbox_apply() (which only
 * confines shell subprocesses via Landlock), this confines individual file
 * tool operations lexically, so it also works on platforms without
 * Landlock (macOS, Windows, kernels without Landlock).
 *
 * Behavior:
 * - AIRY_TOOL_SANDBOX_MODE=off: `resolved` receives `path` verbatim,
 *   return 0 (sandboxing disabled).
 * - Otherwise the workspace is taken from AIRY_TOOL_SANDBOX_WORKSPACE
 *   (canonicalized), falling back to the current working directory.
 * - for_write == 0: `path` must resolve inside the workspace; a missing
 *   path is confined normally and its absence is reported by the
 *   subsequent I/O (e.g. fs_read returns NOT_FOUND, not a permission
 *   error).
 * - for_write == 1: same confinement; `for_write` documents the access
 *   intent (the deepest existing ancestor is canonicalized and the
 *   non-existing tail re-appended, so creating new files inside the
 *   workspace works; the joined result is lexically normalized and must
 *   stay inside the workspace).
 *
 * Returns 0 on success (path confined, `resolved` filled); -1 when the
 * path escapes the workspace or resolution fails (fail-closed). */
int os_sandbox_fs_confine(const char *path, int for_write, char *resolved,
                          size_t resolved_cap);

/* Resolve the canonical workspace root used by fs confinement (the
 * AIRY_TOOL_SANDBOX_WORKSPACE entry canonicalized, or the current
 * working directory when unset). Same resolution rules as
 * os_sandbox_fs_confine(); returns 0 on success, -1 on failure. */
int os_sandbox_fs_workspace(char *ws, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_OS_SANDBOX_H */
