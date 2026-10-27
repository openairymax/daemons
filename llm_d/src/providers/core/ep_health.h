/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* ep_health.h: LLM 端点健康探测机制件（llm_d providers/core，0.1.19 D5） */

#ifndef LLM_D_PROVIDERS_CORE_EP_HEALTH_H
#define LLM_D_PROVIDERS_CORE_EP_HEALTH_H

#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ep_health ep_health_t;

/* 建立探测视图：对 registry 中名匹配 filter 的端点（filter 为空 = 全部）
 * 取 api_base 快照。probe_path 为空时取标准 "/models"。registry 仅在创建
 * 期使用，handle 建立后不再引用它。返回 NULL 表示 registry 为空或内存不足。 */
ep_health_t *ep_health_create(provider_registry_t *reg, const char *filter, const char *probe_path);

void ep_health_destroy(ep_health_t *h);

/* 对全部已知端点发起一次幂等只读探测并刷新可达性/延迟。运行在 daemon
 * 线程池 worker 上（探测量级为端点数量级，远小于池容量）。
 *
 * deep != 0 时追加深度探针（1-token 生成）：/models 只证明路由存在，深度
 * 探针才证明权重已加载、推理可用。深度探针由策略层按周期倍率触发（如每
 * 5 个轻探测周期一次），机制件不自持周期。深度结果保持粘滞：轻探测成功
 * 但最近一次深度探测失败，端点仍判不可达，直到下一次深度探测通过。
 * 端点无可用模型名时深度探针不适用，判定只由轻探测决定。 */
void ep_health_sweep(ep_health_t *h, int deep);

/* 上报一次请求侧冷加载观测：first_ms 超过冷加载阈值时按 OLLAMA_KEEP_ALIVE
 * 给出建议并返回 1（该端点若在探测视图内则一并记入）；未超阈值返回 0。 */
int ep_health_cold(ep_health_t *h, const char *provider, const char *model, double first_ms);

/* 探测结果汇聚出口（0.1.20 D2-5）：sweep 每完成一个端点的最终可达性判定，
 * 在锁外回调一次，供装配域接进端点健康面（llm_rtr_provider_note）。本件不
 * include router 头，保持 providers/core 对 router 的单向无依赖。
 * 同一轮 sweep 内每端点至多回调一次；端点已从视图消失则不回调。 */
typedef void (*ep_health_sink_t)(void *ud, const char *provider, int reachable);

void ep_health_sink(ep_health_t *h, ep_health_sink_t sink, void *ud);

/* 快照序列化为 JSON 数组（provider/base/status/http_code/latency_ms/deep/cold，
 * 冷加载端点附加 cold_model/first_ms）。deep 取 -1 未探 / 0 失败 / 1 通过。
 * 返回串由调用方以 AIRY_FREE 释放；视图为空或内存不足返回 NULL。 */
char *ep_health_json(ep_health_t *h);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_EP_HEALTH_H */
