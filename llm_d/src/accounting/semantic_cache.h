// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file semantic_cache.h
 * @brief Accounting domain: cross-process semantic cache (L1) hosted by mem_d.
 *
 * 本件是"语义缓存"策略的机制面：把 mem_d 的 cache_get / cache_put 方法
 * 封装为两个窄接口，RPC 方法名与 mem.sock 解析均不越出本域。
 * 是否启用由调用方按请求级声明（cacheable）判定，本层不做准入门禁。
 */

#ifndef AIRY_RT_LLM_SEMANTIC_CACHE_H
#define AIRY_RT_LLM_SEMANTIC_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 查询 mem_d 语义缓存
 * @return 1 命中（*out_json 为响应 JSON，调用方 AIRY_FREE）；0 未命中或不可用
 */
int llm_semantic_cache_fetch(const char *text, const char *model, char **out_json);

/**
 * @brief 写入 mem_d 语义缓存（失败仅 DEBUG，不阻断主流程）
 */
void llm_semantic_cache_save(const char *text, const char *model, const char *resp_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SEMANTIC_CACHE_H */
