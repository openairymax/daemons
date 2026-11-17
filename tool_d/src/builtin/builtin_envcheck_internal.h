/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_envcheck_internal.h
 * @brief 环境可行性预检：包管理器 / 发行版 / 仓库 / 命令存在性探针。
 *
 * 执行前的可行性判定与执行后的失败归因共用同一组探针。判定的边界是
 * 「可确定的不可行」：只有输出命中确定的失败标记（包不存在、命令不存在、
 * 仓库与网络不可达）才判 `AIRY_ERR_EXEC_ENV`，其余一律交回通用失败路径，
 * 避免把普通命令失败误报成环境问题。归因里附探针事实（发行版、版本、
 * 包管理器、仓库状态），使上层能把「重试」换成「换路径或告知用户」。
 *
 * 通道开关 AIRY_PRECHECK=off 关闭归因（默认开启）。
 */

#ifndef AIRY_RT_TOOL_BUILTIN_ENVCHECK_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_ENVCHECK_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENV_PRECHECK_SWITCH "AIRY_PRECHECK"
#define ENV_REASON_MAX 256
#define ENV_DISTRO_MAX 64
#define ENV_VERSION_MAX 64
#define ENV_PKGMGR_MAX 16

/* 一次探针采集的机器事实：四个命令位回答「有没有」，repos_ready 回答
 * 「包源是否可读」。字段全部为定长，便于在栈上持有且不依赖分配。 */
typedef struct {
    char distro[ENV_DISTRO_MAX];
    char version[ENV_VERSION_MAX];
    char pkg_mgr[ENV_PKGMGR_MAX];
    int has_dnf;
    int has_rpm;
    int has_systemctl;
    int repos_ready;
} env_probe_t;

/* 开关：默认开启；off/0/false/no 关闭（大小写不敏感）。 */
int env_precheck_enabled(void);

/* 采集探针事实。读 /etc/os-release（缺失回落 /usr/lib/os-release）、
 * 在 PATH 中探 dnf/rpm/systemctl、检查仓库目录或源文件可读。
 * 采集失败不改变控制流：字段保持空串/0，供归因写 unknown。
 * 成功 AIRY_OK。 */
int env_probe_collect(env_probe_t *p);

/* 命令存在性：含 '/' 时按路径判可执行，否则沿 PATH 查找。
 * 返回 1 存在 / 0 不存在 / -1 无法判定（平台不支持）。 */
int env_probe_program(const char *name);

/* 纯函数：判定一次非 0 退出的失败是否属于「环境不可行」。
 * 命中确定标记时把归因写入 reason 并返回 AIRY_ERR_EXEC_ENV；
 * 否则返回 AIRY_OK（调用方按原口径回填通用失败）。
 * exit_code == 0、reason 为空或 output 为空时不判定。 */
int env_classify(int exit_code, const char *output, const env_probe_t *p, char *reason,
                 size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_ENVCHECK_INTERNAL_H */
