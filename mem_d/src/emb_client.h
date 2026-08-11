/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file emb_client.h
 * @brief Memory 服务可选 embedding 后端客户端（OpenAI 兼容 /embeddings 接口）
 *
 * 作为 TF-IDF 向量检索的可选增强：
 * - 通过环境变量 AIRY_MEM_EMBEDDING_URL / AIRY_MEM_EMBEDDING_KEY 配置
 * - POST {url}/embeddings（model=text-embedding-3-small）
 * - 有配置时使用 embedding 余弦相似度检索；调用失败自动降级 TF-IDF
 * - 仅在编译期定义了 AIRY_HAS_CURL 时可用（libcurl），否则客户端保持禁用
 */

#ifndef MEM_EMB_CLIENT_H
#define MEM_EMB_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *url;
    char *api_key;
    int enabled;
    int healthy;
    uint64_t last_fail_time;
    uint64_t retry_after_ms;
} mem_emb_client_t;

/**
 * @brief 初始化 embedding 客户端（读取环境变量；未配置时保持禁用）
 * @return AIRY_SUCCESS 或错误码（未配置不算错误）
 */
int mem_emb_client_init(mem_emb_client_t *client);

/**
 * @brief 释放客户端资源（url/api_key 等）
 */
void mem_emb_client_destroy(mem_emb_client_t *client);

/**
 * @brief 判断当前是否应尝试调用 embedding（启用且处于冷却期外）
 */
int mem_emb_should_try(const mem_emb_client_t *client);

/**
 * @brief 获取文本 embedding 向量
 * @param client 客户端
 * @param text 文本
 * @param out_vec 输出向量（调用方负责 AIRY_FREE），失败置 NULL
 * @param out_dim 输出维度，失败置 0
 * @return AIRY_SUCCESS 成功；失败返回错误码并标记 unhealthy（触发冷却降级）
 */
int mem_emb_embed(mem_emb_client_t *client, const char *text, float **out_vec, size_t *out_dim);

/**
 * @brief 计算两个 embedding 的余弦相似度，钳制到 [0,1]
 */
float mem_emb_cosine(const float *a, const float *b, size_t dim);

#endif /* MEM_EMB_CLIENT_H */
