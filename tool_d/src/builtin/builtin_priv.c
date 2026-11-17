// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_priv.c
 * @brief 特权执行通道：白名单、通道开关、计划装配与出沙箱执行。
 *
 * shell_run 的每个子进程都套 Landlock + no_new_privs，提权结构上不可达。
 * 本域给出唯一显式通道，判定顺序固定为：通道开关 → 白名单 → 参数形状 →
 * euid → 审计 → 执行。任一环节不成立即拒绝，不留半开状态。
 */

#include "builtin/builtin_limits_internal.h"
#include "builtin/builtin_priv_internal.h"

#ifndef _WIN32

#include <strings.h>
#include <unistd.h>

/* 程序白名单：模型传的是短名，这里给出唯一对应的绝对路径与参数族。
 * 扩展一个程序 = 本表加一行 + 对应族的校验器支持，不接受表外输入。 */
static const priv_prog_t k_priv_progs[] = {
    {"dnf", "/usr/bin/dnf", PRIV_FAMILY_PKG},
    {"rpm", "/usr/bin/rpm", PRIV_FAMILY_PKG},
    {"systemctl", "/usr/bin/systemctl", PRIV_FAMILY_SVC},
};

static int priv_is_off(const char *v)
{
    static const char *const off[] = {"off", "0", "false", "no", NULL};
    for (int i = 0; off[i]; ++i) {
        if (strcasecmp(v, off[i]) == 0)
            return 1;
    }
    return 0;
}

int priv_enabled(void)
{
    const char *v = getenv(PRIV_ENV_SWITCH);
    return (v && v[0]) ? !priv_is_off(v) : 1;
}

const priv_prog_t *priv_lookup(const char *name)
{
    if (!name || !name[0])
        return NULL;
    for (size_t i = 0; i < sizeof(k_priv_progs) / sizeof(k_priv_progs[0]); ++i) {
        if (strcmp(name, k_priv_progs[i].name) == 0)
            return &k_priv_progs[i];
    }
    return NULL;
}

#else /* _WIN32 */

int priv_enabled(void)
{
    return 0;
}

const priv_prog_t *priv_lookup(const char *name)
{
    (void)name;
    return NULL;
}

#endif /* _WIN32 */
