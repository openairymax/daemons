// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_persist.c
 * @brief JSONL file persistence for mem_service.
 *
 * Records are stored line-delimited JSON in ${AIRY_DATA_DIR}/agentrt/memory/mem.jsonl.
 * Append is O(1); full rewrite uses tmp-file + rename for crash safety.
 * Extracted from service.c to isolate platform-specific I/O code.
 */

#include "mem_persist.h"
#include "airy_memory.h"
#include "io.h"
#include "safe_utf8.h"
#include "svc_logger.h"
#include "platform.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MEM_JSONL_FILENAME "mem.jsonl"

/* ── JSON escape helper ──────────────────────────────────────────────── */

/* Escape JSON string literal content (excluding the outer quotes).
 * Returns a malloc'd buffer (caller AIRY_FREE) and the written length.
 * On failure *out = NULL and returns 0. */
static size_t mem_json_escape(const char *in, size_t len, char **out)
{
    if (!out)
        return 0;
    *out = NULL;
    if (!in || len == 0) {
        *out = (char *)AIRY_MALLOC(1);
        if (*out)
            (*out)[0] = '\0';
        return 0;
    }
    size_t cap = len * 6 + 1;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return 0;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  buf[pos++] = '\\'; buf[pos++] = '"';  break;
        case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
        case '\n': buf[pos++] = '\\'; buf[pos++] = 'n';  break;
        case '\t': buf[pos++] = '\\'; buf[pos++] = 't';  break;
        case '\r': buf[pos++] = '\\'; buf[pos++] = 'r';  break;
        case '\b': buf[pos++] = '\\'; buf[pos++] = 'b';  break;
        case '\f': buf[pos++] = '\\'; buf[pos++] = 'f';  break;
        default:
            if (c < 0x20) {
                int w = snprintf(buf + pos, cap - pos, "\\u%04x", c);
                if (w > 0)
                    pos += (size_t)w;
            } else {
                buf[pos++] = (char)c;
            }
            break;
        }
    }
    buf[pos] = '\0';
    *out = buf;
    return pos;
}

/* ── Path resolution ─────────────────────────────────────────────────── */

int mem_jsonl_path_resolve(char **out_path)
{
    if (!out_path)
        return AIRY_ERR_INVALID_PARAM;
    *out_path = NULL;

    /* 持久记忆数据落 data/（run/ 仅承载 socket/pid 等易失运行时文件；
     * 记忆随会话留存，属持久数据，与 heapstore/hall 同一分区）。 */
    const char *dir = airy_data_dir();
    if (!dir || !dir[0])
        return AIRY_ERR_INVALID_PARAM;

    /* 确保完整父目录存在：仅 airy_mkdir_p(dir) 只建了 data 目录，
     * agentrt/memory 子目录缺失时首次运行 append/rewrite 静默失败
     * （errno=2），记忆不落盘。必须递归创建完整父路径。 */
    size_t dir_len = strlen(dir);
    size_t need = dir_len + 1 + strlen("agentrt") + 1 + strlen("memory") + 1 +
                  strlen(MEM_JSONL_FILENAME) + 1;
    char *path = (char *)AIRY_MALLOC(need);
    if (!path)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(path, need, "%s/agentrt/memory/%s", dir, MEM_JSONL_FILENAME);

    char parent[4096];
    snprintf(parent, sizeof(parent), "%s/agentrt/memory", dir);
    (void)airy_mkdir_p(parent);

    *out_path = path;
    return AIRY_SUCCESS;
}

/* ── Record format (single source of truth) ──────────────────────────── */

/* Emit one JSONL record line. Returns -1 when the underlying write failed and
 * 0 when the record was written or skipped (unescapable payload). */
static int mem_jsonl_emit(FILE *f, const mem_record_entry_t *rec)
{
    if (!rec->record_id || !rec->data)
        return 0;

    char *esc = NULL;
    mem_json_escape((const char *)rec->data, rec->len, &esc);
    if (!esc) {
        SVC_LOG_WARN("mem_d persist: json escape failed (len=%zu)", rec->len);
        return 0;
    }

    int rc = fprintf(f,
                     "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":"
                     "%llu,\"len\":%zu}\n",
                     rec->record_id, esc, rec->metadata ? rec->metadata : "null",
                     (unsigned long long)rec->created_at, rec->len);
    AIRY_FREE(esc);
    return rc < 0 ? -1 : 0;
}

/* ── Atomic write (tmp + fsync + rename) ─────────────────────────────── */

typedef bool (*mem_sink_t)(FILE *f, void *ctx);

static bool mem_rename_over(const char *tmp, const char *path)
{
    return airy_file_rename(tmp, path) == 0;
}

/* Write a whole file through @p sink so a crash mid-write never truncates an
 * existing file. Returns true on success. */
static bool mem_atomic_write(const char *path, mem_sink_t sink, void *ctx)
{
    char tmppath[4096];
    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", path) >= (int)sizeof(tmppath)) {
        SVC_LOG_WARN("mem_d persist: tmp path too long (path=%s)", path);
        return false;
    }

    FILE *f = fopen(tmppath, "wb");
    if (!f) {
        SVC_LOG_WARN("mem_d persist: open tmp failed (path=%s errno=%d)", tmppath, errno);
        return false;
    }

    if (!sink(f, ctx) || fflush(f) != 0 || airy_io_sync(f) != 0) {
        SVC_LOG_WARN("mem_d persist: write failed (path=%s errno=%d)", tmppath, errno);
        fclose(f);
        remove(tmppath);
        return false;
    }
    if (fclose(f) != 0) {
        remove(tmppath);
        return false;
    }
    if (!mem_rename_over(tmppath, path)) {
        SVC_LOG_WARN("mem_d persist: rename failed (errno=%d)", errno);
        remove(tmppath);
        return false;
    }
    return true;
}

static bool mem_sink_jsonl(FILE *f, void *ctx)
{
    const mem_service_t *svc = (const mem_service_t *)ctx;

    for (size_t i = 0; i < svc->record_count; i++) {
        if (mem_jsonl_emit(f, &svc->records[i]) < 0)
            return false;
    }
    return true;
}

/* ── Append ──────────────────────────────────────────────────────────── */

void mem_persist_append_record(mem_service_t *svc, const mem_record_entry_t *rec)
{
    if (!svc || !svc->jsonl_path || !rec || !rec->record_id || !rec->data)
        return;

    if (!svc->jsonl_append_fp) {
        svc->jsonl_append_fp = fopen(svc->jsonl_path, "a");
        if (!svc->jsonl_append_fp) {
            SVC_LOG_WARN("mem_d persist: open append failed (path=%s errno=%d)", svc->jsonl_path,
                         errno);
            return;
        }
    }

    if (mem_jsonl_emit(svc->jsonl_append_fp, rec) < 0)
        SVC_LOG_WARN("mem_d persist: append write failed (errno=%d)", errno);
    else if (fflush(svc->jsonl_append_fp) != 0)
        SVC_LOG_WARN("mem_d persist: fflush failed (errno=%d)", errno);
}

/* ── Full rewrite (crash-safe) ───────────────────────────────────────── */

void mem_persist_rewrite_all(mem_service_t *svc)
{
    if (!svc || !svc->jsonl_path)
        return;

    if (svc->jsonl_append_fp) {
        fclose(svc->jsonl_append_fp);
        svc->jsonl_append_fp = NULL;
    }

    /* Crash safety: rewrite through the shared atomic-write path so a crash
     * mid-rewrite never truncates the whole memory store. */
    if (!mem_atomic_write(svc->jsonl_path, mem_sink_jsonl, svc))
        SVC_LOG_WARN("mem_d persist: rewrite failed (path=%s)", svc->jsonl_path);
}

/* ── Load ────────────────────────────────────────────────────────────── */

/* One-time repair of records written by earlier versions, which truncated
 * memory by byte index and could therefore persist a multi-byte UTF-8
 * character cut in half. Such a record, once recalled and sent to a model
 * provider, is rejected with HTTP 400 "invalid unicode code point" while the
 * user only sees a generic connectivity failure.
 *
 * Replace every invalid sequence with U+FFFD, keeping a full backup of the
 * original file first; without a successful backup the store is left
 * untouched. Returns true when *content was replaced by a repaired buffer
 * (the caller owns and must free the previous buffer). */
static bool mem_persist_repair_utf8(mem_service_t *svc, char **content, size_t *len)
{
    if (!svc || !svc->jsonl_path || !content || !*content || !len || *len == 0)
        return false;

    size_t in_len = *len;
    if (in_len > (SIZE_MAX - 1) / 3)
        return false;

    /* Each invalid byte may expand to a 3-byte U+FFFD. */
    size_t cap = in_len * 3 + 1;
    char *clean = (char *)AIRY_MALLOC(cap);
    if (!clean)
        return false;

    size_t written = utf8_sanitize(*content, in_len, clean, cap);
    if (written == in_len && memcmp(clean, *content, in_len) == 0) {
        AIRY_FREE(clean); /* already valid UTF-8, nothing to repair */
        return false;
    }

    char bak[4096];
    if (snprintf(bak, sizeof(bak), "%s.bak", svc->jsonl_path) >= (int)sizeof(bak)) {
        SVC_LOG_WARN("mem_d persist: repair backup path too long, skip repair");
        AIRY_FREE(clean);
        return false;
    }

    if (airy_io_write_file(bak, *content, in_len) != 0) {
        SVC_LOG_WARN("mem_d persist: repair backup write failed, skip repair");
        AIRY_FREE(clean);
        return false;
    }

    if (airy_io_write_file(svc->jsonl_path, clean, written) != 0) {
        SVC_LOG_WARN("mem_d persist: repair rewrite failed, backup kept at %s", bak);
        AIRY_FREE(clean);
        return false;
    }

    SVC_LOG_WARN("mem_d persist: repaired legacy invalid UTF-8 records (%zu -> %zu bytes), backup at %s",
                 in_len, written, bak);

    AIRY_FREE(*content);
    *content = clean;
    *len = written;
    return true;
}

static void mem_record_release(mem_record_entry_t *rec)
{
    AIRY_FREE(rec->record_id);
    AIRY_FREE(rec->data);
    AIRY_FREE(rec->metadata);
    rec->record_id = NULL;
    rec->data = NULL;
    rec->metadata = NULL;
}

/* 还原 metadata 与时间戳：metadata 以未格式化 JSON 串落盘，非对象/空值视为无。 */
static void mem_record_meta(mem_record_entry_t *rec, const cJSON *root)
{
    cJSON *meta = cJSON_GetObjectItem(root, "metadata");
    if (meta && !cJSON_IsNull(meta)) {
        char *s = cJSON_PrintUnformatted(meta);
        if (s) {
            rec->metadata = AIRY_STRDUP(s);
            cJSON_free(s);
        }
    }
    cJSON *cat = cJSON_GetObjectItem(root, "created_at");
    rec->score = 0.0f;
    rec->created_at =
        (cat && cJSON_IsNumber(cat)) ? (uint64_t)cat->valuedouble : (uint64_t)time(NULL);
}

/* 解码一条 JSONL 记录并追加到索引；返回是否真正装入。 */
static bool mem_record_decode(mem_service_t *svc, const cJSON *root)
{
    cJSON *rid = cJSON_GetObjectItem(root, "record_id");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(rid) || !cJSON_IsString(data) || svc->record_count >= svc->max_records)
        return false;

    size_t idx = svc->record_count;
    mem_record_entry_t *rec = &svc->records[idx];
    cJSON *len_item = cJSON_GetObjectItem(root, "len");
    size_t data_len = (len_item && cJSON_IsNumber(len_item))
                          ? (size_t)len_item->valuedouble
                          : strlen(data->valuestring);
    if (data_len == 0)
        data_len = strlen(data->valuestring);

    rec->record_id = AIRY_STRDUP(rid->valuestring);
    rec->data = AIRY_MALLOC(data_len + 1);
    if (!rec->record_id || !rec->data) {
        mem_record_release(rec);
        return false;
    }
    __builtin_memcpy(rec->data, data->valuestring, data_len);
    ((char *)rec->data)[data_len] = '\0';
    rec->len = data_len;
    mem_record_meta(rec, root);

    if (hindex_put(&svc->record_index, rec->record_id, idx) != AIRY_SUCCESS) {
        mem_record_release(rec);
        return false;
    }
    mem_record_build_vector(svc, rec);
    svc->record_count++;
    return true;
}

/* 逐行切分已清洗的存储内容，坏行跳过不影响其余记录。 */
static size_t mem_load_lines(mem_service_t *svc, char *content, size_t read_len)
{
    size_t loaded = 0;
    char *line = content;

    for (size_t i = 0; i <= read_len; i++) {
        if (i != read_len && content[i] != '\n')
            continue;
        content[i] = '\0';
        size_t llen = strlen(line);
        while (llen > 0 && line[llen - 1] == '\r')
            line[--llen] = '\0';
        if (llen > 0) {
            cJSON *root = cJSON_Parse(line);
            if (!root) {
                SVC_LOG_WARN("mem_d persist: skip malformed jsonl line");
            } else {
                if (mem_record_decode(svc, root))
                    loaded++;
                cJSON_Delete(root);
            }
        }
        line = content + i + 1;
    }
    return loaded;
}

void mem_persist_load_existing(mem_service_t *svc)
{
    if (!svc || !svc->jsonl_path)
        return;

    char *content = NULL;
    size_t read_len = 0;
    if (airy_io_read_file(svc->jsonl_path, &content, &read_len) != 0 || read_len == 0)
        return;

    /* 存量数据清洗：早期版本按字节截断写坏的记录在这里一次性修复
     * （备份原文件后原地改写），随后按清洗后的内容加载。 */
    (void)mem_persist_repair_utf8(svc, &content, &read_len);

    size_t loaded = mem_load_lines(svc, content, read_len);
    AIRY_FREE(content);
    if (loaded > 0)
        SVC_LOG_INFO("mem_d persist: loaded %zu records from %s", loaded, svc->jsonl_path);
}
