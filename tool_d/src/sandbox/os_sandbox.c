// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

//
// @file os_sandbox.c
// @brief shell_run sandbox policy mapping (config -> commons native sandbox)
//
// Policy layer only: maps AIRY_TOOL_SANDBOX_* environment/configuration to
// airy_native_sandbox_t and delegates the mechanism (Landlock filesystem
// rules + seccomp BPF deny-list) to commons/platform_sandbox (the SSoT,
// §204). This file additionally owns:
// - rlimit shaping (RLIMIT_AS/NOFILE/NPROC/CPU/CORE) - policy knobs absent
//   from the commons mechanism on purpose
// - the cross-platform lexical path confinement for file tools
//   (os_sandbox_fs_confine), which also protects hosts without Landlock
//
// Fail-closed semantics (require_landlock / STRICT without Landlock) are
// decided here before handing control to the mechanism layer.
//
// Platform support: sandbox enforcement is Linux-only (macOS/Windows keep
// OS_SANDBOX_MODE_OFF semantics); fs confinement is cross-platform.

#include "os_sandbox.h"
#include "platform_sandbox.h"
#include "airy_memory.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__

#include <limits.h>
#include <sys/resource.h>

void os_sandbox_cfg_from_env(os_sandbox_cfg_t *cfg)
{
    AIRY_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->mode = OS_SANDBOX_MODE_WORKSPACE;
    cfg->net_access = 1;

    const char *mode = getenv("AIRY_TOOL_SANDBOX_MODE");
    if (mode) {
        if (strcmp(mode, "off") == 0) {
            cfg->mode = OS_SANDBOX_MODE_OFF;
        } else if (strcmp(mode, "strict") == 0) {
            cfg->mode = OS_SANDBOX_MODE_STRICT;
            cfg->net_access = 0;
        } else {
            cfg->mode = OS_SANDBOX_MODE_WORKSPACE;
        }
    }

    const char *ws = getenv("AIRY_TOOL_SANDBOX_WORKSPACE");
    if (ws && ws[0]) {
        char real[PATH_MAX];
        if (realpath(ws, real) != NULL) {
            snprintf(cfg->workspace, sizeof(cfg->workspace), "%s", real);
        } else {
            snprintf(cfg->workspace, sizeof(cfg->workspace), "%s", ws);
        }
    } else if (getcwd(cfg->workspace, sizeof(cfg->workspace)) == NULL) {
        cfg->workspace[0] = '\0';
    }

    const char *net = getenv("AIRY_TOOL_SANDBOX_NET");
    if (net) {
        cfg->net_access = (strcmp(net, "0") != 0 && strcmp(net, "false") != 0) ? 1 : 0;
    }

    const char *rq = getenv("AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK");
    if (rq) {
        cfg->require_landlock = (strcmp(rq, "0") != 0 && strcmp(rq, "false") != 0) ? 1 : 0;
    }
}

static int os_sandbox_apply_rlimits(const os_sandbox_cfg_t *cfg)
{
    struct rlimit rl;

    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    (void)setrlimit(RLIMIT_CORE, &rl);

    if (cfg->mem_limit_mb > 0) {
        rl.rlim_cur = cfg->mem_limit_mb * 1024ULL * 1024ULL;
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_AS, &rl) != 0) {
            return -1;
        }
    }
    if (cfg->nofile_limit > 0) {
        rl.rlim_cur = cfg->nofile_limit;
        rl.rlim_max = cfg->nofile_limit;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            return -1;
        }
    }
    if (cfg->nproc_limit > 0) {
        rl.rlim_cur = cfg->nproc_limit;
        rl.rlim_max = cfg->nproc_limit;
        if (setrlimit(RLIMIT_NPROC, &rl) != 0) {
            return -1;
        }
    }
    if (cfg->cpu_limit_sec > 0) {
        rl.rlim_cur = cfg->cpu_limit_sec;
        rl.rlim_max = cfg->cpu_limit_sec;
        if (setrlimit(RLIMIT_CPU, &rl) != 0) {
            return -1;
        }
    }
    return 0;
}

/* STRICT 模式只读白名单：系统可执行/库/配置目录 + /dev（设备节点；
 * /dev/null 的写在共用 rw 尾表中单独叠加）。 */
static const char *const k_strict_ro_paths[] = {
    "/bin", "/sbin", "/usr", "/lib", "/lib64", "/lib32", "/libx32", "/etc", "/opt", "/dev", NULL,
};

/* 两模式共用写路径尾表：/tmp（临时文件）+ /dev/null（shell 重定向
 * 常规目标）；workspace 头插在 apply 中，空 workspace 时仅尾表生效。 */
static const char *const k_rw_tail[] = { "/tmp", "/dev/null", NULL };

/* Policy mapping (§204): cfg -> airy_native_sandbox_t, then delegate to
 * the commons mechanism. Fail-closed decisions (STRICT / require_landlock
 * without Landlock) stay here; the mechanism degrades per-layer. */
int os_sandbox_apply(const os_sandbox_cfg_t *cfg)
{
    if (!cfg || cfg->mode == OS_SANDBOX_MODE_OFF) {
        return 0;
    }

    int ll_ok = airy_native_sandbox_landlock_available();
    if (cfg->mode == OS_SANDBOX_MODE_STRICT && !ll_ok) {
        SVC_LOG_ERROR("os_sandbox: strict mode requires Landlock, unavailable");
        return -1;
    }
    if (!ll_ok && cfg->require_landlock) {
        SVC_LOG_ERROR("os_sandbox: Landlock unavailable and "
                      "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1, refusing to "
                      "run unsandboxed");
        return -1;
    }

    if (os_sandbox_apply_rlimits(cfg) != 0) {
        SVC_LOG_ERROR("os_sandbox: rlimit apply failed");
        return -1;
    }

    const char *rw[4];
    size_t n = 0;
    if (cfg->workspace[0]) {
        rw[n++] = cfg->workspace;
    }
    for (size_t i = 0; k_rw_tail[i] != NULL; i++) {
        rw[n++] = k_rw_tail[i];
    }
    rw[n] = NULL;

    airy_native_sandbox_t sb;
    airy_native_sandbox_init(&sb);
    sb.enabled = 1;
    sb.deny_network = !cfg->net_access;
    sb.global_read = (cfg->mode == OS_SANDBOX_MODE_WORKSPACE) ? 1 : 0;
    sb.ro_paths = (cfg->mode == OS_SANDBOX_MODE_STRICT) ? k_strict_ro_paths : NULL;
    sb.rw_paths = rw;

    if (airy_native_sandbox_apply(&sb) != 0) {
        SVC_LOG_ERROR("os_sandbox: native sandbox apply failed");
        return -1;
    }
    if (!ll_ok) {
        SVC_LOG_WARN("os_sandbox: Landlock unavailable, degraded to "
                     "seccomp-only isolation; set "
                     "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1 to fail closed "
                     "instead");
    }
    return 0;
}

#else /* !__linux__ */
void os_sandbox_cfg_from_env(os_sandbox_cfg_t *cfg)
{
    AIRY_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->mode = OS_SANDBOX_MODE_OFF;
}

int os_sandbox_apply(const os_sandbox_cfg_t *cfg)
{
    if (cfg && cfg->require_landlock) {
        SVC_LOG_ERROR("os_sandbox: non-Linux platform has no sandbox and "
                      "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1, refusing to run");
        return -1;
    }
    SVC_LOG_WARN("os_sandbox: non-Linux platform, sandbox unavailable "
                 "(requested mode=%d); tool execution runs unsandboxed",
                 cfg ? cfg->mode : -1);
    return 0;
}

#endif /* __linux__ */

/* ------------------------------------------------------------------
 * File-tool path confinement (cross-platform).
 *
 * Unlike os_sandbox_apply() (which confines shell subprocesses through
 * Landlock on Linux), this confines individual file-tool operations
 * lexically so it also protects hosts without Landlock: macOS, Windows
 * and Linux kernels without Landlock all get workspace confinement for
 * fs_read/fs_write/fs_edit/fs_list/fs_delete/fs_glob/fs_grep.
 * ------------------------------------------------------------------ */

#ifndef OS_FS_PATH_MAX
#ifdef PATH_MAX
#define OS_FS_PATH_MAX PATH_MAX
#else
#define OS_FS_PATH_MAX 4096
#endif
#endif

/* Confine depth cap for the strip-segment loop (deepest existing
 * ancestor search); also bounds the number of remembered tail segments. */
#define OS_FS_CONFINE_MAX_DEPTH 128

static int fs_confine_realpath(const char *path, char *out, size_t cap)
{
#if defined(_WIN32)
    (void)cap;
    return _fullpath(out, path, cap) != NULL ? 0 : -1;
#else
    (void)cap;
    return realpath(path, out) != NULL ? 0 : -1;
#endif
}

/* Canonical workspace for file-tool confinement. Same environment inputs
 * as os_sandbox_cfg_from_env(), read independently so non-Linux hosts
 * (where cfg_from_env pins mode to OFF) still get confinement. */
static int fs_confine_workspace(char *ws, size_t cap)
{
    const char *env = getenv("AIRY_TOOL_SANDBOX_WORKSPACE");
    if (env && env[0] != '\0') {
        if (fs_confine_realpath(env, ws, cap) == 0) {
            return 0;
        }
        /* Not canonicalizable (e.g. dangling path): use verbatim, the
         * prefix check below then fails closed for anything that does
         * not literally live underneath it. */
        int pr = snprintf(ws, cap, "%s", env);
        return (pr >= 0 && (size_t)pr < cap) ? 0 : -1;
    }
    if (getcwd(ws, cap) == NULL) {
        return -1;
    }
    return 0;
}

/* Candidate must be the workspace itself or live underneath it. */
static int fs_confine_check_prefix(const char *cand, const char *ws, const char *orig_path,
                                   char *resolved, size_t resolved_cap)
{
    size_t n = strlen(ws);
#if defined(_WIN32)
    if (_strnicmp(cand, ws, n) != 0) {
#else
    if (strncmp(cand, ws, n) != 0) {
#endif
        SVC_LOG_WARN("os_sandbox: path escapes workspace sandbox: '%s'", orig_path);
        return -1;
    }
    char next = cand[n];
    if (next != '\0' && next != '/'
#if defined(_WIN32)
        && next != '\\'
#endif
    ) {
        /* Sibling name sharing a prefix (e.g. /ws vs /ws-private). */
        SVC_LOG_WARN("os_sandbox: path escapes workspace sandbox: '%s'", orig_path);
        return -1;
    }
    int pr = snprintf(resolved, resolved_cap, "%s", cand);
    if (pr < 0 || (size_t)pr >= resolved_cap) {
        return -1;
    }
    return 0;
}

int os_sandbox_fs_confine(const char *path, int for_write, char *resolved, size_t resolved_cap)
{
    if (!path || path[0] == '\0' || !resolved || resolved_cap == 0) {
        return -1;
    }

    const char *mode_env = getenv("AIRY_TOOL_SANDBOX_MODE");
    if (mode_env && strcmp(mode_env, "off") == 0) {
        int pr = snprintf(resolved, resolved_cap, "%s", path);
        return (pr >= 0 && (size_t)pr < resolved_cap) ? 0 : -1;
    }

    char ws[OS_FS_PATH_MAX];
    if (fs_confine_workspace(ws, sizeof(ws)) != 0 || ws[0] == '\0') {
        SVC_LOG_WARN("os_sandbox: cannot resolve workspace, refusing path '%s'", path);
        return -1;
    }

    int is_abs = (path[0] == '/');
#if defined(_WIN32)
    if ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) {
        if (path[1] == ':' || path[1] == '/' || path[1] == '\\') {
            is_abs = 1;
        }
    }
#endif

    char work[OS_FS_PATH_MAX];
    int pr;
    if (is_abs) {
        pr = snprintf(work, sizeof(work), "%s", path);
    } else {
        pr = snprintf(work, sizeof(work), "%s/%s", ws, path);
    }
    if (pr < 0 || (size_t)pr >= sizeof(work)) {
        return -1;
    }

    /* Strip trailing separators so realpath sees the real leaf. */
    size_t wl = strlen(work);
    while (wl > 1 && (work[wl - 1] == '/'
#if defined(_WIN32)
                      || work[wl - 1] == '\\'
#endif
                      )) {
        work[--wl] = '\0';
    }

    /* Fast path: the full path already exists - realpath canonicalizes
     * it (resolving any symlink) and the prefix check confines it. */
    char base[OS_FS_PATH_MAX];
    if (fs_confine_realpath(work, base, sizeof(base)) == 0) {
        return fs_confine_check_prefix(base, ws, path, resolved, resolved_cap);
    }

    /* The path does not exist (yet). For reads this is not an error at
     * this layer: confinement only decides *where* access is allowed;
     * existence is reported by the subsequent I/O (a read of a missing
     * file yields ENOENT -> NOT_FOUND instead of a misleading permission
     * error). Canonicalize the deepest existing ancestor, remember the
     * stripped tail, then re-join lexically. */
    char *segs[OS_FS_CONFINE_MAX_DEPTH];
    size_t seg_count = 0;

    for (int depth = 0; depth < OS_FS_CONFINE_MAX_DEPTH; depth++) {
        if (fs_confine_realpath(work, base, sizeof(base)) == 0) {
            break;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            SVC_LOG_WARN("os_sandbox: cannot resolve path '%s' (errno=%d)", path, errno);
            return -1;
        }
        char *cut = strrchr(work, '/');
#if defined(_WIN32)
        char *cut_bk = strrchr(work, '\\');
        if (cut_bk > cut) {
            cut = cut_bk;
        }
#endif
        if (!cut) {
            SVC_LOG_WARN("os_sandbox: cannot anchor path '%s' inside workspace", path);
            return -1;
        }
        if (seg_count >= OS_FS_CONFINE_MAX_DEPTH) {
            return -1;
        }
        segs[seg_count++] = cut + 1;
        if (cut == work) {
            work[1] = '\0'; /* keep the root itself ("/") */
        } else {
            *cut = '\0';
        }
        if (work[0] == '\0') {
            return -1;
        }
    }
    if (seg_count >= OS_FS_CONFINE_MAX_DEPTH) {
        SVC_LOG_WARN("os_sandbox: path '%s' too deep to confine", path);
        return -1;
    }

    /* Lexical normalization of base + tail: "." dropped, ".." pops one
     * segment, popping past the root means escape. Without this step a
     * tail containing ".." could re-form a string that passes the
     * textual prefix check while actually resolving outside. */
    char norm[OS_FS_PATH_MAX];
    pr = snprintf(norm, sizeof(norm), "%s", base);
    if (pr < 0 || (size_t)pr >= sizeof(norm)) {
        return -1;
    }
    for (size_t i = seg_count; i-- > 0;) {
        const char *s = segs[i];
        if (s[0] == '\0' || strcmp(s, ".") == 0) {
            continue;
        }
        if (strcmp(s, "..") == 0) {
            char *p = strrchr(norm, '/');
            if (!p) {
                SVC_LOG_WARN("os_sandbox: path escapes workspace sandbox: '%s'", path);
                return -1;
            }
            if (p == norm) {
                norm[1] = '\0'; /* "/.." stays at the root */
            } else {
                *p = '\0';
            }
            continue;
        }
        {
            /* snprintf must not alias source and destination. */
            char joined[OS_FS_PATH_MAX];
            pr = snprintf(joined, sizeof(joined), "%s/%s", norm, s);
            if (pr < 0 || (size_t)pr >= sizeof(joined)) {
                return -1;
            }
            AIRY_MEMCPY(norm, joined, (size_t)pr + 1);
        }
    }

    return fs_confine_check_prefix(norm, ws, path, resolved, resolved_cap);
}

int os_sandbox_fs_workspace(char *ws, size_t cap)
{
    if (!ws || cap == 0) {
        return -1;
    }
    return fs_confine_workspace(ws, cap);
}
