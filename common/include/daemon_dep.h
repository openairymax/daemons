/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_dep.h
 * @brief Daemon hard-dependency declaration and startup probe (governance face).
 *
 * 0.1.19 §7.2 运行不变量 3：硬依赖缺失必须降级态显式上报，禁止静默 ready。
 * 不变量 2：健康 = 端到端可达（不得以启动快照或文件存在性冒充健康）。
 *
 * 一个 daemon 以数据方式声明自己的依赖（名字 + required/optional），在
 * ServiceDiscovery 引导完成之后调用本模块一次：
 *   - daemon_dep_probe()  ：经 SD 解析每个声明的依赖是否可达
 *   - daemon_dep_report() ：required 缺失时发出降级信号（WARN + hall issue 事件）
 * 健康面（handle_health_check）按请求重新探测，因此上报的是当前可达性，
 * 而非启动快照。
 *
 * 本模块是 governance 面的机制件，零策略：谁依赖谁是调用方以数据声明的，
 * 后续由每个 daemon 的 .manifest（required/optional）生成/接管。
 * 零堆分配：句柄是调用方持有的普通结构，无 OOM 失败模式。
 */

#ifndef AIRY_RT_DAEMON_DEP_H
#define AIRY_RT_DAEMON_DEP_H

#include "error.h" /* airy_err_t */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单个 daemon 可声明的依赖上限。 */
#define DAEMON_DEP_MAX 8

/* 前向声明：探测接收调用方持有的 sd_helper_t
 *（commons/utils/sd/service_discovery_helper.h）。 */
typedef struct sd_helper_s sd_helper_t;

/** 一条依赖声明。 */
typedef struct {
    const char *name; /**< 在 SD 中注册的服务名（如 "llm_d"） */
    bool required;    /**< true：缺失即降级 */
} daemon_dep_spec_t;

/** 一组依赖的探测状态（调用方持有，无堆分配）。 */
typedef struct {
    daemon_dep_spec_t specs[DAEMON_DEP_MAX];
    bool reachable[DAEMON_DEP_MAX];
    size_t count;
    bool probed;
} daemon_dep_t;

/**
 * @brief 将依赖声明表绑定到探测句柄。
 * @param dep    待初始化的句柄（调用方持有）
 * @param specs  声明数组
 * @param count  声明条数（0 <= count <= DAEMON_DEP_MAX）
 * @return AIRY_SUCCESS；dep 为 NULL 或 count>0 而 specs 为 NULL 时返回
 *         AIRY_ERR_INVALID_PARAM；count > DAEMON_DEP_MAX 时返回
 *         AIRY_ERR_BUFFER_TOO_SMALL
 */
int daemon_dep_init(daemon_dep_t *dep, const daemon_dep_spec_t *specs, size_t count);

/**
 * @brief 经 ServiceDiscovery 解析全部声明依赖的可达性。
 *
 * 可重复调用：每次重新从 SD 注册表刷新可达性，因此健康面观察到的是当前
 * 状态，而非启动快照。
 *
 * @param dep 已初始化的句柄
 * @param sdh daemon 持有的 SD helper；为 NULL 时所有依赖标记为不可达
 *            （fail-closed），不算错误
 * @return AIRY_SUCCESS 表示探测完成（即使存在缺失依赖——结果由
 *         daemon_dep_ready/daemon_dep_missing 表达）；
 *         dep 为 NULL 时返回 AIRY_ERR_INVALID_PARAM
 */
int daemon_dep_probe(daemon_dep_t *dep, sd_helper_t *sdh);

/**
 * @brief 是否完全就绪：已探测且无 required 缺失。
 */
bool daemon_dep_ready(const daemon_dep_t *dep);

/** @brief 不可达的 required 依赖条数。 */
size_t daemon_dep_missing(const daemon_dep_t *dep);

/** @brief 已声明的依赖条数。 */
size_t daemon_dep_count(const daemon_dep_t *dep);

/**
 * @brief 回读一条依赖声明及其探测结果。
 * @return AIRY_SUCCESS；dep 为 NULL 或 idx >= count 时返回
 *         AIRY_ERR_INVALID_PARAM
 */
int daemon_dep_at(const daemon_dep_t *dep, size_t idx, const char **name, bool *required,
                  bool *reachable);

/**
 * @brief 将缺失的 required 依赖渲染为逗号分隔列表。
 * @param buf 输出缓冲
 * @param cap 缓冲容量（字节）
 * @return 写入的字符数（不含结尾 NUL）；buf 为 NULL 或 cap 为 0 时返回 -1
 */
int daemon_dep_note(const daemon_dep_t *dep, char *buf, size_t cap);

/**
 * @brief required 依赖缺失时发出降级信号。
 *
 * 无缺失时静默（不打日志、不发事件）。否则记录一条 WARN，并以 daemon 名
 * 为分组键写入一条 hall "issue" 事件。
 *
 * @param dep         已探测的句柄
 * @param daemon_name daemon 名（事件流分组键，如 "think_d"）
 * @return 缺失的 required 依赖条数（无缺失为 0）；dep 为 NULL、daemon_name
 *         为 NULL/空或句柄未探测时返回 AIRY_ERR_INVALID_PARAM
 */
int daemon_dep_report(const daemon_dep_t *dep, const char *daemon_name);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_DEP_H */
