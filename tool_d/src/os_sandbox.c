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

#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv)
#define OS_LL_CREATE_RULESET 444
#define OS_LL_ADD_RULE 445
#define OS_LL_RESTRICT_SELF 446
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

        SVC_LOG_WARN("os_sandbox: Landlock unavailable, degraded to "
                     "rlimit+seccomp only (workspace mode)");
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
    (void)cfg;
    return 0;
}

#endif /* __linux__ */
