/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_model_watch.h
 * @brief model.yaml 启动快照 + 请求侧变更检测（0.1.19 §3.5 缺陷 5）。
 *
 * model.yaml 仅在 daemon 启动时一次性载入（llm_d provider 注册表、
 * think_d think 段），用户改文件后 daemon 静默走旧配置、无任何提示。
 * 本机制件在启动时对生效配置文件照快照（size+mtime+FNV-1a 64 内容
 * 摘要），请求侧廉价检测（size/mtime 未变则单次 stat 即返回）漂移，
 * 首次漂移一次性 WARN 日志 + 运行时目录落 stale 标记文件；启动器
 * `airymaxrt status` 读标记面提示「配置已修改，需重启生效」。
 *
 * 语义裁定：
 *   - 热重载不做（§3.5 缺陷 5：0.1.20 若实现则替代，防本版膨胀）
 *   - mtime 变而内容未变（touch）不算漂移，防误报
 *   - 内容 hash 为 FNV-1a 64：非加密摘要，仅用于变更检测
 *
 * 线程安全：单实例内 warned 置位与快照刷新非原子；daemon 侧消费点
 * （llm_service_complete / think_service_process）各自串行化或容忍
 * 双写 WARN（幂等日志 + 幂等标记文件，无状态破坏）。
 */

#ifndef AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H
#define AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char path[1024]; /* 生效配置文件路径（启动时快照对象） */
    char daemon[32]; /* 消费 daemon 名（stale 标记文件后缀） */
    uint64_t size;      /* 快照文件大小（字节） */
    int64_t mtime_sec;  /* 快照 mtime（epoch 秒） */
    int64_t mtime_nsec; /* 快照 mtime（纳秒部分，同秒同长改写可辨） */
    uint64_t hash;      /* 快照内容 FNV-1a 64 摘要 */
    int valid;          /* 1 = 启动快照在位 */
    int warned;         /* 1 = 本次漂移已告警（一次性） */
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

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMONS_COMMON_SVC_MODEL_WATCH_H */
