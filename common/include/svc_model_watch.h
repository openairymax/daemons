/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_model_watch.h
 * @brief model.yaml 启动快照、漂移检测与 SIGHUP 热重载（0.1.19 §3.5 缺陷 5
 *        / 0.1.20 D2-7）。
 *
 * model.yaml 仅在 daemon 启动时一次性载入，用户改文件后旧配置静默生效、
 * 无任何提示。本机制件分三件事，边界严格分开：
 *
 *   检测 —— 启动时照快照（size+mtime+FNV-1a 64 内容摘要）。周期 tick 与
 *           请求侧各有一条廉价检测路径（元数据未变则单次 stat 即返回）。
 *   上报 —— 漂移一次性发 config_drift{file,old_hash,new_hash,applied:false}；
 *           请求侧另有一次 WARN 日志与运行时目录 stale 标记文件，供启动器
 *           status 面聚合。
 *   应用 —— 唯一入口 svc_mw_reload()：解析→校验→原子切换。机制件不做切换
 *           本身，切换经 apply 回调由属主注入（机制/策略分离）。成功发
 *           config_applied{file,hash,generation}；失败发
 *           config_reload_failed{file,reason} 且旧配置原样生效。
 *
 * 语义裁定：
 *   - tick 只检测不应用：应用只由 SIGHUP 触发，改文件不会自动生效
 *   - mtime 变而内容未变（touch）不算漂移，防误报
 *   - 内容 hash 为 FNV-1a 64：非加密摘要，仅用于变更检测
 *   - 漂移消解（内容回到基线）后重置一次性闩锁，后续漂移可再次上报
 *
 * 线程安全：非原子。tick 在事件循环线程执行，请求侧检测在工作线程执行；
 * 两者对 warned/drift_ev 的竞争最坏只产生一次重复上报（幂等事件 + 幂等
 * 标记文件），不破坏状态。故不内嵌互斥量——Windows CRITICAL_SECTION 不可
 * 零初始化，会把本结构拖进 init/destroy 生命周期。
 */

#ifndef AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H
#define AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 缺省检测周期：空闲进程最迟 5 秒内上报一次漂移（D2-7 回归判据）。 */
#define SVC_MW_INTERVAL_MS 5000U

/**
 * @brief 应用回调（属主注入的配置切换入口）
 *
 * @param ud  属主上下文
 * @param why 失败原因回填缓冲（成功时可不写）
 * @param cap why 容量
 * @return 0 已切换；1 忙或退避窗口内（不产生事件）；<0 失败
 */
typedef int (*svc_mw_apply_t)(void *ud, char *why, size_t cap);

typedef struct {
    char path[1024];    /* 生效配置文件路径（NULL 表示沿用，空串表示停用） */
    char daemon[32];    /* 消费 daemon 名（事件分组键 + 标记文件后缀） */
    uint64_t size;      /* 快照文件大小（字节） */
    int64_t mtime_sec;  /* 快照 mtime（epoch 秒） */
    int64_t mtime_nsec; /* 快照 mtime（纳秒部分，同秒同长改写可辨） */
    uint64_t hash;      /* 快照内容 FNV-1a 64 摘要 */
    int valid;          /* 1 = 启动快照在位 */
    int warned;         /* 1 = 本次漂移已告警（请求侧一次性） */
    int drift_ev;       /* 1 = 本次漂移已发 config_drift（tick 侧一次性） */
    int drift_pending;  /* 1 = 漂移未消解，等待 SIGHUP 应用 */
    uint32_t interval_ms; /* tick 周期，0 = 取 SVC_MW_INTERVAL_MS */
    uint64_t last_ms;     /* 上次 tick 时刻（节流基线） */
    uint32_t generation;  /* 成功应用次数（单调递增，事件可对账） */
    svc_mw_apply_t apply; /* 应用回调，NULL = 只检测不应用 */
    void *apply_ud;       /* 应用回调上下文 */
} svc_mw_t;

/**
 * @brief 启动时照快照并清除本 daemon 的 stale 标记
 * @return 0 = 快照在位；<0 = 文件缺席/不可读（valid=0，检测静默停用）
 */
int svc_mw_init(svc_mw_t *w, const char *path, const char *daemon);

/**
 * @brief 请求侧漂移检测（廉价路径：size/mtime 未变单次 stat 返回）
 * @return 1 = 检测到漂移（首次同时 WARN + 落标记）；0 = 无漂移；<0 = 检测不可用
 */
int svc_mw_check(svc_mw_t *w);

/**
 * @brief 挂接应用回调（幂等，可重复调用）
 * @param interval_ms tick 周期，0 = 取 SVC_MW_INTERVAL_MS
 * @return 0 成功；<0 参数非法
 */
int svc_mw_hook(svc_mw_t *w, svc_mw_apply_t apply, void *ud, uint32_t interval_ms);

/**
 * @brief 周期检测：按 interval 节流，漂移首次发 config_drift
 * @param now_ms 调用方时钟（airy_time_ms），便于测试注入
 * @return 1 = 本轮检出漂移；0 = 无漂移或未到周期；<0 = 检测不可用
 */
int svc_mw_tick(svc_mw_t *w, uint64_t now_ms);

/**
 * @brief 应用当前落盘配置（SIGHUP 唯一入口），成功后重照快照并推进世代
 * @return 0 = 已应用；1 = 忙/退避；<0 = 失败（旧配置继续生效）
 */
int svc_mw_reload(svc_mw_t *w);

/**
 * @brief 就地重照快照并清闩锁（不推进世代、不改应用回调）
 * @param path NULL = 沿用原路径；空串 = 停用本监视
 * @return 0 成功；<0 文件不可读（valid=0）
 */
int svc_mw_rebase(svc_mw_t *w, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H */
