// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

//
// @file os_sandbox.c
// @brief shell_run OS-level sandbox implementation (Landlock + seccomp + rlimit)
//
// Implementation notes (mirroring Codex linux-sandbox):
// - Landlock: kernel-LSM user-space filesystem sandbox. Whitelist
//   semantics - after restrict_self, the process can only access explicitly
//   allowed paths; everything else is EACCES. No root needed, no capability
//   dependency; this is the core of shell_run's protection against
//   "writing system dirs / reading unauthorized files".
// - seccomp: BPF blacklist filtering, forbidding privileged and namespace
//   syscalls such as mount/umount2/ptrace/unshare/setns, preventing the
//   sandboxed process from gaining higher privileges.
// - rlimit: RLIMIT_AS/NOFILE/NPROC/CPU/CORE, preventing fork bombs /
//   memory exhaustion.
//
// Landlock ABI constants are defined self-contained (aligned with Linux
// UAPI, not depending on kernel-header versions).
//
// Platform support: Linux only. macOS/Windows compile to an empty
// implementation (OS_SANDBOX_MODE_OFF).

#include "os_sandbox.h"
#include "airy_memory.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__

#include <fcntl.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>

/* Landlock syscall 号按架构 syscall 表（arch/<arch>/syscall*.tbl）：
 * i386 表较 x86_64 少 1（443/444/445）；arm EABI / aarch64 / riscv 与
 * x86_64 一致（444/445/446）。32 位目标缺 __i386__/__arm__ 分支时
 * OS_LL_* 未定义，i686 编译直接报错（i686/armv7l 六架构实测）。 */
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || \
    defined(__arm__) || defined(__riscv)
#if defined(__i386__)
#define OS_LL_CREATE_RULESET 443
#define OS_LL_ADD_RULE 444
#define OS_LL_RESTRICT_SELF 445
#else
#define OS_LL_CREATE_RULESET 444
#define OS_LL_ADD_RULE 445
#define OS_LL_RESTRICT_SELF 446
#endif
#else

#define OS_LL_NO_SUPPORT 1
#endif

#define OS_LL_RULE_PATH_BENEATH 1

/* handled_access_fs permission bits (ABI1, without ABI2 REFER / ABI3
 * TRUNCATE, keeping old-kernel compatibility; unhandled permission bits stay
 * denied by default after restrict) */
#define OS_LL_FS_EXECUTE (1ULL << 0)
#define OS_LL_FS_WRITE_FILE (1ULL << 1)
#define OS_LL_FS_READ_FILE (1ULL << 2)
#define OS_LL_FS_READ_DIR (1ULL << 3)
#define OS_LL_FS_REMOVE_DIR (1ULL << 4)
#define OS_LL_FS_REMOVE_FILE (1ULL << 5)
#define OS_LL_FS_MAKE_CHAR (1ULL << 6)
#define OS_LL_FS_MAKE_DIR (1ULL << 7)
#define OS_LL_FS_MAKE_REG (1ULL << 8)
#define OS_LL_FS_MAKE_SOCK (1ULL << 9)
#define OS_LL_FS_MAKE_FIFO (1ULL << 10)
#define OS_LL_FS_MAKE_BLOCK (1ULL << 11)
#define OS_LL_FS_MAKE_SYM (1ULL << 12)

struct os_ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
};
struct os_ll_path_beneath_attr {
    uint64_t allowed_access;
    int32_t parent_fd;
};

#define LL_FS_READ (OS_LL_FS_READ_FILE | OS_LL_FS_READ_DIR)
#define LL_FS_WRITE                                                                          \
    (OS_LL_FS_WRITE_FILE | OS_LL_FS_REMOVE_DIR | OS_LL_FS_REMOVE_FILE | OS_LL_FS_MAKE_CHAR | \
     OS_LL_FS_MAKE_DIR | OS_LL_FS_MAKE_REG | OS_LL_FS_MAKE_SOCK | OS_LL_FS_MAKE_FIFO |       \
     OS_LL_FS_MAKE_BLOCK | OS_LL_FS_MAKE_SYM)
#define LL_FS_EXEC OS_LL_FS_EXECUTE
#define LL_FS_HANDLED (LL_FS_READ | LL_FS_WRITE | LL_FS_EXEC)

static const char *const k_sys_read_paths[] = {
    "/bin", "/sbin", "/usr", "/lib", "/lib64", "/lib32", "/libx32", "/etc", "/opt", NULL,
};

int os_sandbox_landlock_available(void)
{
#ifdef OS_LL_NO_SUPPORT
    return 0;
#else
    /* The probe must use a non-empty handled_access_fs: an empty ruleset
     * makes the kernel return EINVAL directly, making it impossible to
     * distinguish "unsupported" from "empty attributes". Probe with all ABI1
     * fs permission bits. */
    struct os_ll_ruleset_attr attr;
    AIRY_MEMSET(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LL_FS_HANDLED;
    int fd = (int)syscall(OS_LL_CREATE_RULESET, &attr, sizeof(attr), 0U);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
#endif
}

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

#define SECCOMP_BAN(nr)                              \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))

static int os_sandbox_apply_seccomp(void)
{
    /* Blacklist: privileged syscalls such as namespace/mount/debug/module
     * load. Fetch the syscall nr first, then JEQ per entry; on a hit return
     * EPERM, otherwise allow everything. */
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),

        SECCOMP_BAN(__NR_mount),
        SECCOMP_BAN(__NR_umount2),
        SECCOMP_BAN(__NR_pivot_root),
        SECCOMP_BAN(__NR_ptrace),
        SECCOMP_BAN(__NR_unshare),
        SECCOMP_BAN(__NR_setns),
        SECCOMP_BAN(__NR_kexec_load),
        SECCOMP_BAN(__NR_reboot),
        SECCOMP_BAN(__NR_bpf),
        SECCOMP_BAN(__NR_perf_event_open),
        SECCOMP_BAN(__NR_init_module),
        SECCOMP_BAN(__NR_finit_module),
        SECCOMP_BAN(__NR_delete_module),
        SECCOMP_BAN(__NR_acct),
        SECCOMP_BAN(__NR_swapon),
        SECCOMP_BAN(__NR_swapoff),
        SECCOMP_BAN(__NR_sethostname),
        SECCOMP_BAN(__NR_setdomainname),
        SECCOMP_BAN(__NR_open_by_handle_at),
        SECCOMP_BAN(__NR_name_to_handle_at),
        SECCOMP_BAN(__NR_keyctl),
        SECCOMP_BAN(__NR_add_key),
        SECCOMP_BAN(__NR_request_key),

        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        return -1;
    }
    return 0;
}

static int os_sandbox_open_dir(const char *path)
{
    int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        fd = open(path, O_PATH | O_CLOEXEC);
    }
    return fd;
}

static int os_sandbox_ll_allow(int ruleset_fd, const char *path, uint64_t access)
{
    int dir_fd = os_sandbox_open_dir(path);
    if (dir_fd < 0) {
        return (errno == ENOENT) ? -2 : -1;
    }
    struct os_ll_path_beneath_attr rule;
    AIRY_MEMSET(&rule, 0, sizeof(rule));
    rule.allowed_access = access;
    rule.parent_fd = dir_fd;
    int rc = (int)syscall(OS_LL_ADD_RULE, ruleset_fd, OS_LL_RULE_PATH_BENEATH, &rule, 0U);
    close(dir_fd);
    return (rc == 0) ? 0 : -1;
}

static int os_sandbox_ll_allow_file(int ruleset_fd, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT) ? -2 : -1;
    }
    struct os_ll_path_beneath_attr rule;
    AIRY_MEMSET(&rule, 0, sizeof(rule));
    rule.allowed_access = access;
    rule.parent_fd = fd;
    int rc = (int)syscall(OS_LL_ADD_RULE, ruleset_fd, OS_LL_RULE_PATH_BENEATH, &rule, 0U);
    close(fd);
    return (rc == 0) ? 0 : -1;
}

/* Apply the Landlock filesystem whitelist:
 * - WORKSPACE: global read+exec, workspace/temp dirs writable
 * - STRICT: only system base paths + workspace readable/executable;
 *   workspace/temp dirs writable
 * Both allow /dev read-only and /dev/null writes (common shell redirection
 * targets). */
static int os_sandbox_apply_landlock(const os_sandbox_cfg_t *cfg)
{
    struct os_ll_ruleset_attr attr;
    AIRY_MEMSET(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LL_FS_HANDLED;

    int ruleset_fd = (int)syscall(OS_LL_CREATE_RULESET, &attr, sizeof(attr), 0U);
    if (ruleset_fd < 0) {
        return -1;
    }

    int rc = 0;
    const uint64_t rw = LL_FS_READ | LL_FS_WRITE;
    const uint64_t rx = LL_FS_READ | LL_FS_EXEC;
    const uint64_t w = LL_FS_WRITE;

    if (cfg->mode == OS_SANDBOX_MODE_WORKSPACE) {

        if (os_sandbox_ll_allow(ruleset_fd, "/", rx) != 0) {
            rc = -1;
            goto out;
        }
    } else {

        for (int i = 0; k_sys_read_paths[i] != NULL; i++) {
            int ar = os_sandbox_ll_allow(ruleset_fd, k_sys_read_paths[i], rx);
            if (ar == -1) {
                rc = -1;
                goto out;
            }
        }
    }

    if (cfg->workspace[0]) {
        if (os_sandbox_ll_allow(ruleset_fd, cfg->workspace, rw) != 0) {
            SVC_LOG_ERROR("os_sandbox: allow workspace %s failed", cfg->workspace);
            rc = -1;
            goto out;
        }
    }

    (void)os_sandbox_ll_allow(ruleset_fd, "/tmp", w);

    (void)os_sandbox_ll_allow(ruleset_fd, "/dev", rx);
    (void)os_sandbox_ll_allow_file(ruleset_fd, "/dev/null", OS_LL_FS_WRITE_FILE);

out:
    if (rc == 0) {
        if (syscall(OS_LL_RESTRICT_SELF, ruleset_fd, 0U) != 0) {
            rc = -1;
        }
    }
    close(ruleset_fd);
    return rc;
}

int os_sandbox_apply(const os_sandbox_cfg_t *cfg)
{
    if (!cfg || cfg->mode == OS_SANDBOX_MODE_OFF) {
        return 0;
    }
#ifdef OS_LL_NO_SUPPORT
    if (cfg->require_landlock) {
        SVC_LOG_ERROR("os_sandbox: built without Landlock support and "
                      "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1, refusing to "
                      "run unsandboxed");
        return -1;
    }
    return (cfg->mode == OS_SANDBOX_MODE_STRICT) ? -1 : 0;
#else
    int ll_ok = os_sandbox_landlock_available();

    if (cfg->mode == OS_SANDBOX_MODE_STRICT && !ll_ok) {
        SVC_LOG_ERROR("os_sandbox: strict mode requires Landlock, unavailable");
        return -1;
    }

    if (os_sandbox_apply_rlimits(cfg) != 0) {
        SVC_LOG_ERROR("os_sandbox: rlimit apply failed");
        return -1;
    }

    if (os_sandbox_apply_seccomp() != 0) {
        SVC_LOG_ERROR("os_sandbox: seccomp apply failed");
        return -1;
    }

    if (ll_ok) {
        if (os_sandbox_apply_landlock(cfg) != 0) {
            SVC_LOG_ERROR("os_sandbox: landlock apply failed");
            return -1;
        }
    } else {
        if (cfg->require_landlock) {
            SVC_LOG_ERROR("os_sandbox: Landlock unavailable and "
                          "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1, refusing to "
                          "run with rlimit+seccomp only");
            return -1;
        }
        SVC_LOG_WARN("os_sandbox: Landlock unavailable, degraded to "
                     "rlimit+seccomp only (workspace mode); set "
                     "AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1 to fail closed "
                     "instead");
    }
    return 0;
#endif
}

#else /* !__linux__ */
int os_sandbox_landlock_available(void)
{
    return 0;
}

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
