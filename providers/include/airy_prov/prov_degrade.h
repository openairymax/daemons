/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file prov_degrade.h
 * @brief 部署位阶与降级阶梯契约（0.1.21 B-4）
 *
 * 选路需要一条默认降级阶梯：本机 GPU → 本机 CPU → 远端端点。此前路由
 * 端点优先级恒为 0，阶梯只存在于人脑与配置书写顺序里，GPU 不可用时不会
 * 自动退到 CPU，远端也不会排在本地之后。本头把"部署位阶"独立成唯一判据，
 * 供策略层换算成路由优先级，并在状态面输出 `degraded:<原因>`。
 *
 * 判据只取客观事实：api_base 是否指向本机 loopback、能力位是否声明了
 * 加速器。不对 provider 命名做前缀猜测——命名是用户自由，位置是网络事实。
 *
 * 与两侧的职责边界：
 * - provider_caps 回答"端点宣称能做什么"（能力位）；
 * - ep_health 回答"端点此刻是否可达"（存活判据）；
 * - 本件回答"当前部署落在阶梯哪一级"（位阶），不改判前两者。
 */

#ifndef LLM_D_PROVIDERS_CORE_PROV_DEGRADE_H
#define LLM_D_PROVIDERS_CORE_PROV_DEGRADE_H

#include "airy_prov/provider_rec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 部署位阶：数值即优先级序号，越小越优先。 */
typedef enum {
    PROV_DEG_GPU = 0,    /**< 本机 + 已声明加速器：完整能力。 */
    PROV_DEG_CPU = 1,    /**< 本机 + 未声明加速器：CPU 推理，degraded:local_cpu。 */
    PROV_DEG_REMOTE = 2, /**< 远端端点：本机算力不可用，degraded:remote。 */
    PROV_DEG_NONE = 3    /**< 无从判定（空名或 api_base 缺失）：degraded:unknown。 */
} prov_deg_t;

/**
 * @brief 判定 api_base 是否指向本机 loopback。
 *
 * 接受带 scheme 的完整 URL 或裸 host；命中 127.0.0.1 / localhost / [::1]
 * 且其后紧邻 '\0'、':'、'/'、'?' 时判本机。空值或未知形态返回 0。
 *
 * @param api_base 端点基址（可空）
 * @return 1 本机，0 非本机或无法判定
 */
int prov_host_local(const char *api_base);

/**
 * @brief 计算 provider 的部署位阶。
 *
 * 远端优先按位置判 REMOTE（网络依赖与数据出域不因远端有 GPU 而消失）；
 * 仅本机端点在"已声明加速器"时判 GPU，否则判 CPU。
 *
 * @param prov provider 记录（可空）
 * @return 位阶；无法判定返回 PROV_DEG_NONE
 */
prov_deg_t prov_deg_rank(const provider_t *prov);

/**
 * @brief 位阶的状态面文本。
 *
 * GPU 返回 "gpu"；其余返回 `degraded:<原因>`，原因取 local_cpu / remote /
 * unknown。状态面可直接原样输出，不得把降级位阶报成健康。
 *
 * @param rank 位阶
 * @return 静态字符串
 */
const char *prov_deg_text(prov_deg_t rank);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROV_DEGRADE_H */
