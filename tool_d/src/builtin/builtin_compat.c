// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_compat.c
 * @brief 第三方二进制兼容归因：输出侧 ELF 标记 + 内核日志侧 LSM 标记。
 *
 * 判定只读：命令输出与内核日志尾部各扫一遍，命中即给结论。内核日志
 * 只读文件尾部，不调用 dmesg（不派生进程）。结论与「程序崩溃」显式
 * 分开，并给出可执行的下一步，避免把兼容性问题继续归因成工具缺陷。
 */

#include "builtin/builtin_compat_internal.h"

#include "builtin/builtin_limits_internal.h"

#ifndef _WIN32

#include <strings.h>

static int compat_has_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        if (strncasecmp(p, needle, n) == 0)
            return 1;
    }
    return 0;
}

int compat_klog(char *buf, size_t cap)
{
    if (!buf || cap < 2)
        return -1;
    buf[0] = '\0';
    FILE *f = fopen("/var/log/kern.log", "r");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        return -1;
    }
    long want = (cap - 1 < COMPAT_KLOG_MAX) ? (long)(cap - 1) : (long)COMPAT_KLOG_MAX;
    long start = (end > want) ? end - want : 0;
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)want, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

/* 输出侧标记：命令自己说「跑不起来」的确定说法。权限拒绝只有在
 * 126/127（不可执行 / 找不到）下才算兼容性，普通命令的权限报错不算。 */
static const char *compat_out_mark(int exit_code, const char *output)
{
    if (compat_has_ci(output, "cannot execute binary file"))
        return "cannot execute binary file";
    if (compat_has_ci(output, "bad ELF"))
        return "bad ELF image";
    if (compat_has_ci(output, "Exec format error"))
        return "exec format error";
    if ((exit_code == 126 || exit_code == 127) && compat_has_ci(output, "Permission denied"))
        return "permission denied at exec";
    return NULL;
}

/* 内核侧标记：安全模块拒绝与可执行栈拒绝。短词 "LSM"/"denied"
 * 只在日志尾部匹配，不参与命令输出判定。 */
static const char *compat_kernel_mark(const char *klog)
{
    static const struct {
        const char *needle;
        const char *what;
    } marks[] = {
        {"GNU_STACK", "PT_GNU_STACK executable stack refused"},
        {"apparmor", "AppArmor denial"},
        {"selinux", "SELinux denial"},
        {"landlock", "Landlock denial"},
        {"LSM", "kernel LSM denial"},
        {"denied", "kernel security denial"},
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
        if (compat_has_ci(klog, marks[i].needle))
            return marks[i].what;
    }
    return NULL;
}

int compat_note(int exit_code, const char *output, const char *klog, char *note, size_t cap)
{
    if (exit_code == 0 || !note || cap == 0)
        return 0;
    note[0] = '\0';
    const char *why = compat_out_mark(exit_code, output);
    if (!why)
        why = compat_kernel_mark(klog);
    if (!why)
        return 0;
    snprintf(note, cap,
             "\n[compatibility] not a program crash: %s. The image was rejected by the kernel "
             "security module or its own ELF headers; check `dmesg | tail -30`, the active LSM "
             "policy (apparmor/selinux/landlock) and PT_GNU_STACK, then rebuild the binary or "
             "allow-list it instead of retrying the same command.",
             why);
    return 1;
}

char *compat_append(char *output, const char *note)
{
    if (!note || !note[0])
        return output;
    size_t olen = output ? strlen(output) : 0;
    size_t nlen = strlen(note);
    char *merged = (char *)AIRY_REALLOC(output, olen + nlen + 1);
    if (!merged)
        return output;
    AIRY_MEMCPY(merged + olen, note, nlen + 1);
    return merged;
}

#else /* _WIN32 */

/* Windows 无 LSM/ELF 语义：日志读取不可用（-1），归因不判定。 */
int compat_klog(char *buf, size_t cap)
{
    if (buf && cap)
        buf[0] = '\0';
    return -1;
}

int compat_note(int exit_code, const char *output, const char *klog, char *note, size_t cap)
{
    (void)exit_code;
    (void)output;
    (void)klog;
    if (note && cap)
        note[0] = '\0';
    return 0;
}

char *compat_append(char *output, const char *note)
{
    (void)note;
    return output;
}

#endif /* _WIN32 */
