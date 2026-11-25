// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file semantic_cache.h
 * @brief Accounting domain: cross-process semantic cache (L1) hosted by mem_d.
 *
 * 本件是"语义缓存"策略的机制面：把 mem_d 的 cache_get / cache_put 方法
 * 封装为两个窄接口，RPC 方法名与 mem.sock 解析均不越出本域。
 * 是否启用由调用方按请求级声明（cacheable）判定，本层不做准入门禁。
 *
 * N-6 命名空间：调用方通过 ns_hint 声明采样参数指纹（可 NULL），与契约
 * 版本拼成 cache_ns 随请求下发；mem_d 将其纳入键派生，避免换版模型或参数
 * 变更后旧结果被命中。作用域只覆盖版本与请求级参数，模型维度由 mem_d 既有
 * 键（text|model_id）承担；chat 模板名在本域不可见，模板契约变化须把
 * AIRY_SEMCACHE_NS 版本串 +1（见 semantic_cache.c）。
 *
 * 升级语义：llm_d 总会带上当前版本作用域，故升级前写入的条目不会被新请求
 * 复用——这是有意的失效，避免旧模型/旧参数的答案污染新版本；mem_d 侧对
 * 缺少 cache_ns 的老请求仍按旧键工作，两者互不冲突。
 */

#ifndef AIRY_RT_LLM_SEMANTIC_CACHE_H
#define AIRY_RT_LLM_SEMANTIC_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 命中（*out_json 为响应 JSON，调用方 AIRY_FREE）；0 未命中或不可用。
 * ns_hint 为调用方采样参数指纹（可 NULL = 仅按契约版本作用域）。 */
int llm_semantic_cache_fetch(const char *text, const char *model, const char *ns_hint,
                             char **out_json);

/* 失败仅 DEBUG，不阻断主流程。ns_hint 语义同 fetch，读写两侧须同值。 */
void llm_semantic_cache_save(const char *text, const char *model, const char *ns_hint,
                             const char *resp_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SEMANTIC_CACHE_H */
