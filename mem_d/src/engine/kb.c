// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file kb.c
 * @brief Knowledge-base service layer (2.1.2.3 RAG 一等抽象).
 *
 * 开发者通过 kb.* 命名空间把文档摄入（自动 UTF-8 安全分块）、在库内
 * 检索、整库删除与列出，复用既有 record 存储 / TF-IDF+embedding 混合检索。
 * 每条 chunk 记录的 metadata 携带 {"kb_id":..,"doc_id":..,"chunk":N}。
 *
 * Extracted from service.c to keep the KB-specific logic self-contained.
 */

#include "service.h"
#include "mem_persist.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <string.h>

#define MEM_KB_DEFAULT_CHUNK_SIZE 512

/* ── KB metadata helpers ─────────────────────────────────────────────── */

/* 解析 record 的 metadata JSON，返回其中 kb_id 字符串（静态缓冲）。
 * 非 KB 记录（无 kb_id 字段）返回 NULL。 */
static const char *mem_rec_kb_id(const mem_record_entry_t *rec)
{
    if (!rec || !rec->metadata || !rec->metadata[0])
        return NULL;
    cJSON *root = cJSON_Parse(rec->metadata);
    if (!root)
        return NULL;
    cJSON *kb = cJSON_GetObjectItem(root, "kb_id");
    const char *id = (cJSON_IsString(kb) && kb->valuestring) ? kb->valuestring : NULL;
    if (id) {
        static char s_kb_buf[256];
        size_t n = strlen(id);
        if (n >= sizeof(s_kb_buf))
            n = sizeof(s_kb_buf) - 1;
        __builtin_memcpy(s_kb_buf, id, n);
        s_kb_buf[n] = '\0';
        cJSON_Delete(root);
        return s_kb_buf;
    }
    cJSON_Delete(root);
    return NULL;
}

static int mem_rec_in_kb(const mem_record_entry_t *rec, const char *kb_id)
{
    const char *id = mem_rec_kb_id(rec);
    return id && strcmp(id, kb_id) == 0;
}

/* UTF-8 安全分块：从 off 起取至多 chunk_size 字节，若切点落在多字节
 * 序列的连续字节（0x80-0xBF）中间则回退到该序列首字节，保证 chunk
 * 不以半个字符结尾。返回本块字节数（>0）；文本取尽返回 0。 */
static size_t mem_kb_chunk_len(const unsigned char *text, size_t len, size_t off,
                               size_t chunk_size)
{
    size_t end = off + chunk_size;
    if (end >= len) {
        return len - off;
    }
    size_t p = end;
    while (p > off && (text[p] & 0xC0) == 0x80)
        p--;
    return p - off;
}

static int mem_kb_build_meta(const char *kb_id, const char *doc_id, size_t chunk, char **out_meta)
{
    cJSON *meta = cJSON_CreateObject();
    if (!meta)
        return AIRY_ERR_OUT_OF_MEMORY;
    cJSON_AddStringToObject(meta, "kb_id", kb_id);
    cJSON_AddStringToObject(meta, "doc_id", doc_id ? doc_id : "");
    cJSON_AddNumberToObject(meta, "chunk", (double)chunk);
    char *s = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!s)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_meta = s;
    return AIRY_SUCCESS;
}

/* ── Public KB API ───────────────────────────────────────────────────── */

int mem_service_kb_ingest(mem_service_t *svc, const char *kb_id, const char *doc_id,
                          const void *text, size_t len, size_t chunk_size, size_t *out_count)
{
    if (!svc || !svc->initialized || !kb_id || !kb_id[0] || !text || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    if (chunk_size == 0)
        chunk_size = MEM_KB_DEFAULT_CHUNK_SIZE;

    const unsigned char *bytes = (const unsigned char *)text;
    size_t off = 0;
    size_t written = 0;
    size_t chunk_idx = 0;
    int rc = AIRY_SUCCESS;

    while (off < len) {
        size_t clen = mem_kb_chunk_len(bytes, len, off, chunk_size);
        if (clen == 0)
            break;

        char *meta = NULL;
        rc = mem_kb_build_meta(kb_id, doc_id, chunk_idx, &meta);
        if (rc != AIRY_SUCCESS)
            break;

        mem_write_request_t req = {.data = bytes + off, .len = clen, .metadata = meta};
        char *rid = NULL;
        int wret = mem_service_write(svc, &req, &rid);
        AIRY_FREE(meta);
        AIRY_FREE(rid);

        if (wret != AIRY_SUCCESS) {
            rc = wret;
            break;
        }
        written++;
        chunk_idx++;
        off += clen;
    }

    if (out_count)
        *out_count = written;
    return rc;
}

int mem_service_kb_search(mem_service_t *svc, const char *kb_id, const char *query,
                          uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_filtered(svc, kb_id, query, limit, out_hits, out_count);
}

int mem_service_kb_delete(mem_service_t *svc, const char *kb_id, size_t *out_deleted)
{
    if (!svc || !svc->initialized || !kb_id || !kb_id[0])
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);

    /* 逐条交换删除：mem_remove_record_at 将尾部记录搬入当前位置，
     * 因此 i 不可自增（须重查当前位置），保证数组始终紧凑、
     * 无游离记录（record_count 语义正确，destroy 可完整回收）。 */
    size_t deleted = 0;
    for (size_t i = 0; i < svc->record_count;) {
        if (mem_rec_in_kb(&svc->records[i], kb_id)) {
            mem_remove_record_at(svc, i);
            deleted++;
        } else {
            i++;
        }
    }
    if (deleted > 0)
        mem_persist_rewrite_all(svc);

    airy_mtx_unlock(&svc->lock);
    if (out_deleted)
        *out_deleted = deleted;
    SVC_LOG_INFO("mem.kb_delete: kb=%s deleted=%zu", kb_id, deleted);
    return AIRY_SUCCESS;
}

int mem_service_kb_list(mem_service_t *svc, char ***out_kb_ids, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_kb_ids || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_kb_ids = NULL;
    *out_count = 0;

    airy_mtx_lock(&svc->lock);
    char **ids = NULL;
    size_t count = 0;
    for (size_t i = 0; i < svc->record_count; i++) {
        const char *kb = mem_rec_kb_id(&svc->records[i]);
        if (!kb)
            continue;
        int dup = 0;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(ids[j], kb) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            char **grown = (char **)AIRY_REALLOC(ids, (count + 1) * sizeof(char *));
            if (!grown) {
                for (size_t j = 0; j < count; j++)
                    AIRY_FREE(ids[j]);
                AIRY_FREE(ids);
                airy_mtx_unlock(&svc->lock);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            ids = grown;
            ids[count] = AIRY_STRDUP(kb);
            if (!ids[count]) {
                for (size_t j = 0; j < count; j++)
                    AIRY_FREE(ids[j]);
                AIRY_FREE(ids);
                airy_mtx_unlock(&svc->lock);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            count++;
        }
    }
    airy_mtx_unlock(&svc->lock);

    *out_kb_ids = ids;
    *out_count = count;
    return AIRY_SUCCESS;
}

void mem_kb_list_free(char **kb_ids, size_t count)
{
    if (!kb_ids)
        return;
    for (size_t i = 0; i < count; i++)
        AIRY_FREE(kb_ids[i]);
    AIRY_FREE(kb_ids);
}
