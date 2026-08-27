/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file vector.h
 * @brief Internal vector-search interface of the memory service (own
 *        TF-IDF, no external deps).
 *
 * Provides lightweight vector search for mem_d:
 * - tokenizer: English split into lowercase words (stopwords removable),
 *   Chinese split into single chars plus adjacent char bigrams
 * - each memory record builds a term-frequency vector (TF) cached in
 *   memory on write; JSONL persistence keeps the original text unchanged,
 *   vectors are rebuilt from JSONL on restart
 * - global document-frequency (DF) table with smoothed IDF; search computes
 *   TF-IDF cosine similarity in real time
 */

#ifndef MEM_VECTOR_INTERNAL_H
#define MEM_VECTOR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>


typedef struct {
    char *term;
    float tf;
} mem_vec_term_t;

typedef struct {
    mem_vec_term_t *terms;
    size_t term_count;
    size_t term_capacity;
    size_t total_tokens;
} mem_tfidf_vec_t;


typedef struct {
    char *key;
    size_t df;
    int state;
} mem_df_entry_t;

typedef struct {
    mem_df_entry_t *entries;
    size_t capacity;
    size_t count;
} mem_df_table_t;


/**
 * @brief Tokenize a text and build its term-frequency vector (terms
 *        de-duplicated, tf is the raw count).
 * @param text Text (may be NULL/empty, yielding an empty vector)
 * @param len Text length (bytes)
 * @param out Output vector (no init needed; freed via mem_vec_destroy on success)
 * @return AIRY_SUCCESS or error code
 */
int mem_vec_build(const char *text, size_t len, mem_tfidf_vec_t *out);

/** @brief Free the term-frequency vector resources and zero the struct. */
void mem_vec_destroy(mem_tfidf_vec_t *vec);

/** @brief Initialize the global document-frequency table. */
int mem_df_init(mem_df_table_t *tbl, size_t capacity);

/** @brief Free the document-frequency table resources. */
void mem_df_destroy(mem_df_table_t *tbl);

/** @brief Add a document vector to the global DF table (df+1 per term,
 *        inserting first occurrences). */
void mem_df_add_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc);

/** @brief Remove a document vector from the global DF table (df-1 per term,
 *        deleting entries when df hits zero). */
void mem_df_remove_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc);

/**
 * @brief TF-IDF cosine similarity of query and document, in [0,1].
 *
 * IDF is computed in real time from the current global DF table (document
 * set changes after writes make weights slightly stale, but real-time
 * computation keeps search quality correct). Returns 0 when either vector
 * is empty or no terms are shared.
 *
 * @param query Query vector
 * @param doc Document vector
 * @param df_tbl Global document-frequency table
 * @param doc_count Current document count (N)
 */
float mem_vec_cosine(const mem_tfidf_vec_t *query, const mem_tfidf_vec_t *doc,
                     const mem_df_table_t *df_tbl, size_t doc_count);

/** @brief English stopword test (pass lowercase terms). */
int mem_vec_is_stopword(const char *term);

#endif /* MEM_VECTOR_INTERNAL_H */
