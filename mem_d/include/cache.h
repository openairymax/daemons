/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cache.h
 * @brief Semantic cache service (mem.cache_* namespace).
 *
 * 语义缓存：跨会话、跨时刻的 LLM 响应复用（省 Token、降延迟）。
 * 实现 AirymaxRT 13-semantic-cache-context-ledger.md 第 3 章：
 *   - L0 精确命中：SHA-256(canonical_text + model_id) 精确键（O(1)）
 *   - L1 语义命中：归一化 Jaccard token 集合相似度 + 长度比（可配阈值）
 *   - LRU + TTL 双维度淘汰，条目数/字节数双容量上限
 *   - 仅缓存 cacheable 调用（由调用方决定），命中附加 cache_hit/cache_id 元数据
 */

#ifndef AIRY_RT_MEM_CACHE_H
#define AIRY_RT_MEM_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mem_cache mem_cache_t;

/** @brief 缓存统计。 */
typedef struct {
    size_t entries;     /**< 当前条目数 */
    size_t hits;        /**< 累计命中（L0+L1） */
    size_t misses;      /**< 累计未命中 */
    double hit_rate;    /**< 命中率 hits/(hits+misses) */
    size_t evictions;   /**< 累计淘汰数（TTL 过期 + LRU） */
    size_t bytes;       /**< 当前占用字节（text+response 合计） */
} mem_cache_stats_t;

/**
 * @brief 创建语义缓存。
 * @param max_entries    条目数上限（默认 4096）
 * @param max_bytes      字节数上限（默认 64 MiB）
 * @param default_ttl_ms 默认 TTL（默认 3600_000，0=不过期）
 * @param semantic_threshold L1 语义命中阈值（默认 0.85）
 */
mem_cache_t *mem_cache_create(size_t max_entries, size_t max_bytes,
                              uint64_t default_ttl_ms, double semantic_threshold);

/** @brief 销毁缓存。 */
void mem_cache_destroy(mem_cache_t *cache);

/**
 * @brief 写入缓存条目。
 * @param cache     Cache
 * @param text      规范化请求文本（canonical text）
 * @param response  缓存响应（JSON 序列化字符串）
 * @param model_id  所属模型（命中须同模型）
 * @param ttl_ms    覆盖默认 TTL（0=用默认）
 * @param out_cache_id 返回 32 位十六进制 cache_id（调用方 AIRY_FREE）
 * @param out_exact_key 返回 SHA-256 精确键（调用方 AIRY_FREE）
 * @return AIRY_SUCCESS 或错误码
 */
int mem_cache_put(mem_cache_t *cache, const char *text, const char *response,
                  const char *model_id, uint64_t ttl_ms,
                  char **out_cache_id, char **out_exact_key);

/**
 * @brief 查询缓存（L0 精确 → L1 语义）。
 * @param cache     Cache
 * @param text      请求文本
 * @param model_id  当前模型
 * @param threshold L1 阈值覆盖（<=0 用缓存默认值）
 * @param out_hit   命中标记（1 命中 / 0 未命中）
 * @param out_score 匹配得分（L0=1.0；L1=jaccard 加权）
 * @param out_cache_id 命中条目的 cache_id（未命中为 NULL；调用方 AIRY_FREE）
 * @param out_response 命中响应（未命中为 NULL；调用方 AIRY_FREE）
 * @return AIRY_SUCCESS
 */
int mem_cache_get(mem_cache_t *cache, const char *text, const char *model_id,
                  double threshold, int *out_hit, double *out_score,
                  char **out_cache_id, char **out_response);

/**
 * @brief 删除指定缓存条目。
 * @return AIRY_SUCCESS（不存在也视为成功，out_deleted=0）
 */
int mem_cache_del(mem_cache_t *cache, const char *cache_id, int *out_deleted);

/** @brief 缓存统计（命中率供 monit_d observe 模块聚合）。 */
void mem_cache_stats(mem_cache_t *cache, mem_cache_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_CACHE_H */
