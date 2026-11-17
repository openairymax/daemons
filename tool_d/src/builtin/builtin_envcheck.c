// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_envcheck.c
 * @brief 环境可行性预检：探针采集与失败归因的实现。
 *
 * 采只读事实（os-release、PATH 可执行位、仓库是否可读），不做网络探活，
 * 也不派生任何进程——预检本身不能成为新的失败源。归因只认确定的输出
 * 标记，命中即把事实写进 reason，由调用方改判 ERR_EXEC_ENV。
 */

#include "builtin/builtin_envcheck_internal.h"

#include "builtin/builtin_limits_internal.h"

#ifndef _WIN32

#include <strings.h>
#include <unistd.h>

static int env_is_off(const char *v)
{
    static const char *const off[] = {"off", "0", "false", "no", NULL};
    for (int i = 0; off[i]; ++i) {
        if (strcasecmp(v, off[i]) == 0)
            return 1;
    }
    return 0;
}

int env_precheck_enabled(void)
{
    const char *v = getenv(ENV_PRECHECK_SWITCH);
    return (v && v[0]) ? !env_is_off(v) : 1;
}

/* 从 os-release 的一行里取 key= 的值：去引号、去行尾空白。 */
static void env_take_kv(const char *line, const char *key, char *dst, size_t cap)
{
    if (!line || !dst || cap == 0)
        return;
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0)
        return;
    const char *v = line + klen;
    size_t n = strlen(v);
    while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ' || v[n - 1] == '"'))
        --n;
    if (n >= 2 && v[0] == '"') {
        ++v;
        n -= 2;
    }
    if (n >= cap)
        n = cap - 1;
    AIRY_MEMCPY(dst, v, n);
    dst[n] = '\0';
}

static void env_read_os_release(env_probe_t *p)
{
    FILE *f = fopen("/etc/os-release", "r");
    if (!f)
        f = fopen("/usr/lib/os-release", "r");
    if (!f) {
        snprintf(p->distro, sizeof(p->distro), "unknown");
        snprintf(p->version, sizeof(p->version), "unknown");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        env_take_kv(line, "ID=", p->distro, sizeof(p->distro));
        env_take_kv(line, "VERSION_ID=", p->version, sizeof(p->version));
    }
    fclose(f);
    if (!p->distro[0])
        snprintf(p->distro, sizeof(p->distro), "unknown");
    if (!p->version[0])
        snprintf(p->version, sizeof(p->version), "unknown");
}

static int env_path_has(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !path[0])
        path = "/usr/local/bin:/usr/bin:/bin";
    size_t nlen = strlen(name);
    const char *seg = path;
    while (*seg) {
        const char *end = strchr(seg, ':');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        if (len > 0 && len + nlen + 2 <= 512) {
            char full[512];
            AIRY_MEMCPY(full, seg, len);
            full[len] = '/';
            AIRY_MEMCPY(full + len + 1, name, nlen + 1);
            if (access(full, X_OK) == 0)
                return 1;
        }
        if (!end)
            break;
        seg = end + 1;
    }
    return 0;
}

int env_probe_program(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0 ? 1 : 0;
    return env_path_has(name);
}

static void env_pick_pkg_mgr(env_probe_t *p)
{
    if (p->has_dnf)
        snprintf(p->pkg_mgr, sizeof(p->pkg_mgr), "dnf");
    else if (p->has_rpm)
        snprintf(p->pkg_mgr, sizeof(p->pkg_mgr), "rpm");
    else if (p->has_systemctl)
        snprintf(p->pkg_mgr, sizeof(p->pkg_mgr), "systemctl");
    else
        snprintf(p->pkg_mgr, sizeof(p->pkg_mgr), "none");
}

int env_probe_collect(env_probe_t *p)
{
    if (!p)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(p, 0, sizeof(*p));
    env_read_os_release(p);
    p->has_dnf = env_path_has("dnf");
    p->has_rpm = env_path_has("rpm");
    p->has_systemctl = env_path_has("systemctl");
    /* 仓库可达性只判本地可读：/etc/yum.repos.d 为 dnf 系列，
     * /etc/apt/sources.list 为 deb 系列；两者皆无即判不可读。 */
    p->repos_ready = (access("/etc/yum.repos.d", R_OK) == 0 ||
                      access("/etc/apt/sources.list", R_OK) == 0)
                         ? 1
                         : 0;
    env_pick_pkg_mgr(p);
    return AIRY_OK;
}

static int env_has(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* 判定顺序固定：包不存在 → 命令不存在 → 网络不可达 → 仓库不可达。
 * 同一输出命中多条时按此优先级取第一条，归因不重复堆叠。 */
static const char *env_cause(const char *output)
{
    if (env_has(output, "No match for argument") || env_has(output, "Unable to find a match") ||
        (env_has(output, "No package") && env_has(output, "available")))
        return "package-not-found";
    if (env_has(output, ": command not found"))
        return "command-not-found";
    if (env_has(output, "Could not resolve host"))
        return "network-unreachable";
    if (env_has(output, "Failed to download metadata") || env_has(output, "Cannot download"))
        return "repository-unreachable";
    return NULL;
}

int env_classify(int exit_code, const char *output, const env_probe_t *p, char *reason,
                 size_t cap)
{
    if (exit_code == 0 || !output || !output[0] || !reason || cap == 0)
        return AIRY_OK;
    const char *cause = env_cause(output);
    if (!cause)
        return AIRY_OK;
    snprintf(reason, cap, "environment unmet (%s): distro=%s version=%s pkg_mgr=%s repos=%s",
             cause, (p && p->distro[0]) ? p->distro : "unknown",
             (p && p->version[0]) ? p->version : "unknown",
             (p && p->pkg_mgr[0]) ? p->pkg_mgr : "unknown",
             (p && p->repos_ready) ? "ready" : "unreadable");
    return AIRY_ERR_EXEC_ENV;
}

#else /* _WIN32 */

/* Windows 无 os-release / PATH 可执行位语义，预检保持关闭：
 * 采集返回空事实，归因不判定，命令存在性无法判定（-1）。 */
int env_precheck_enabled(void) { return 1; }

int env_probe_collect(env_probe_t *p)
{
    if (!p)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(p, 0, sizeof(*p));
    return AIRY_OK;
}

int env_probe_program(const char *name)
{
    (void)name;
    return -1;
}

int env_classify(int exit_code, const char *output, const env_probe_t *p, char *reason,
                 size_t cap)
{
    (void)exit_code;
    (void)output;
    (void)p;
    (void)reason;
    (void)cap;
    return AIRY_OK;
}

#endif /* _WIN32 */
