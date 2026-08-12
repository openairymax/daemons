/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file emb_client.h
 * @brief Optional embedding backend client for the memory service
 *        (OpenAI-compatible /embeddings endpoint).
 *
 * Optional enhancement over TF-IDF vector search:
 * - Configured via env vars AIRY_MEM_EMBEDDING_URL / AIRY_MEM_EMBEDDING_KEY
 * - POST {url}/embeddings (model=text-embedding-3-small)
 * - When configured, search uses embedding cosine similarity; on call
 *   failure it auto-degrades to TF-IDF
 * - Only available when AIRY_HAS_CURL is defined at compile time (libcurl);
 *   otherwise the client stays disabled
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
 * @brief Initialize the embedding client (reads env vars; stays disabled
 *        when unconfigured).
 * @return AIRY_SUCCESS or error code (unconfigured is not an error)
 */
int mem_emb_client_init(mem_emb_client_t *client);

/** @brief Release client resources (url/api_key, etc.). */
void mem_emb_client_destroy(mem_emb_client_t *client);

/** @brief Decide whether an embedding call should be attempted now
 *        (enabled and outside the cooldown window). */
int mem_emb_should_try(const mem_emb_client_t *client);

/**
 * @brief Get the embedding vector of a text.
 * @param client Client
 * @param text Text
 * @param out_vec Output vector (caller AIRY_FREEs), NULL on failure
 * @param out_dim Output dimension, 0 on failure
 * @return AIRY_SUCCESS on success; on failure returns an error code and
 *         marks unhealthy (triggering cooldown degradation)
 */
int mem_emb_embed(mem_emb_client_t *client, const char *text, float **out_vec, size_t *out_dim);

/** @brief Cosine similarity of two embeddings, clamped to [0,1]. */
float mem_emb_cosine(const float *a, const float *b, size_t dim);

#endif /* MEM_EMB_CLIENT_H */
