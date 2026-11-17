/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_compat_internal.h
 * @brief 第三方二进制兼容归因：把「一启动就崩」翻译成兼容性结论。
 *
 * 第三方二进制（如 wemeet 的 4 个库带 PT_GNU_STACK RWE）被内核安全模块
 * 拒绝时，进程表现为信号死亡或 126/127 退出，与真正的程序崩溃在上层
 * 无法区分。本域在失败路径上做只读归因：先看命令输出，再看内核日志尾部，
 * 命中 LSM 拒绝或可执行栈拒绝即追加「兼容性问题 + 处置建议」，不改退出码。
 *
 * 归因是纯增强：日志缺失、无命中、开关关闭都不影响原失败语义。
 */

#ifndef AIRY_RT_TOOL_BUILTIN_COMPAT_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_COMPAT_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMPAT_NOTE_MAX 384
#define COMPAT_KLOG_MAX 8192

/* 读内核日志尾部（最多 COMPAT_KLOG_MAX 字节）。只读文件、不派生进程，
 * 避免归因本身触发新的执行面。缺失或读取失败返回 -1，buf 置空串。 */
int compat_klog(char *buf, size_t cap);

/* 判定一次失败是否带兼容性特征：1 命中并写 note / 0 无命中。
 * 输出侧认 ELF 与权限拒绝，日志侧认 LSM 与可执行栈标记；
 * exit_code == 0、note 为空时不判定。 */
int compat_note(int exit_code, const char *output, const char *klog, char *note, size_t cap);

/* 把 note 追加到命令输出尾部（保留原输出）。返回新指针；
 * 分配失败返回原指针，原输出不受损。 */
char *compat_append(char *output, const char *note);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_COMPAT_INTERNAL_H */
