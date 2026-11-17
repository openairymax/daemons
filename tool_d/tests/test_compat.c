// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_compat.c
 * @brief C-27 第三方二进制兼容归因注入测试：输出侧 ELF 标记、内核侧
 *        LSM/GNU_STACK 标记、126/127 权限拒绝、无标记不误判与日志缺失。
 *
 * 内核日志用例使用合成 dmesg 行，避免依赖宿主是否留存 kern.log；
 * 真实 wemeet 样本的复验记 gate:runtime。
 */

#include "builtin/builtin_compat_internal.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_note(void)
{
    char note[COMPAT_NOTE_MAX];

    /* 输出侧：ELF 头部拒绝与执行格式错误。 */
    assert(compat_note(1, "sh: ./wemeet: cannot execute binary file: Exec format error", "", note,
                       sizeof(note)) == 1);
    assert(strstr(note, "not a program crash") != NULL);
    assert(strlen(note) < COMPAT_NOTE_MAX);

    assert(compat_note(1, "error while loading: bad ELF magic", "", note, sizeof(note)) == 1);

    /* 内核侧：LSM 拒绝（合成 dmesg 行）。 */
    const char *lsm =
        "kernel: audit: type=1400 apparmor=\"DENIED\" operation=\"exec\" profile=\"unconfined\"";
    assert(compat_note(127, "Segmentation fault", lsm, note, sizeof(note)) == 1);
    assert(strstr(note, "AppArmor") != NULL);

    /* 内核侧：PT_GNU_STACK 可执行栈拒绝。 */
    const char *stack = "wemeet[2410]: segfault at 0 ... GNU_STACK: executable stack refused";
    assert(compat_note(1, "", stack, note, sizeof(note)) == 1);
    assert(strstr(note, "PT_GNU_STACK") != NULL);

    /* 126/127 + Permission denied 归兼容性；普通退出码下不算。 */
    assert(compat_note(126, "sh: ./wemeet: Permission denied", "", note, sizeof(note)) == 1);
    assert(compat_note(1, "ls: Permission denied", "", note, sizeof(note)) == 0);

    /* 无标记与 exit 0 一律不判定。 */
    assert(compat_note(127, "some ordinary failure", "", note, sizeof(note)) == 0);
    assert(compat_note(0, "cannot execute binary file", "", note, sizeof(note)) == 0);
    assert(compat_note(1, "cannot execute binary file", "", NULL, 0) == 0);
    printf("PASS note\n");
}

static void test_klog(void)
{
    char buf[COMPAT_KLOG_MAX];
    int n = compat_klog(buf, sizeof(buf));
    /* 宿主可能不留存 kern.log：缺失返回 -1 且 buf 为空串，属预期。 */
    assert(n >= 0 || n == -1);
    if (n < 0)
        assert(buf[0] == '\0');
    assert(compat_klog(NULL, 0) == -1);
    printf("PASS klog\n");
}

static void test_append(void)
{
    char *out = AIRY_STRDUP("command output");
    assert(out != NULL);
    char *merged = compat_append(out, "\n[compatibility] note");
    assert(merged != NULL);
    assert(strstr(merged, "command output") != NULL);
    assert(strstr(merged, "[compatibility]") != NULL);
    AIRY_FREE(merged);

    char *from_null = compat_append(NULL, "note");
    assert(from_null != NULL && strcmp(from_null, "note") == 0);
    AIRY_FREE(from_null);

    char *keep = AIRY_STRDUP("x");
    assert(keep != NULL);
    assert(compat_append(keep, "") == keep);
    AIRY_FREE(keep);
    printf("PASS append\n");
}

int main(void)
{
    test_note();
    test_klog();
    test_append();
    printf("ALL PASS test_compat\n");
    return 0;
}
