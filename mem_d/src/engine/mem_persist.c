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
#include "svc_logger.h"
#include "platform.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

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

    char *data_esc = NULL;
    mem_json_escape((const char *)rec->data, rec->len, &data_esc);
    if (!data_esc) {
        SVC_LOG_WARN("mem_d persist: json escape failed (len=%zu)", rec->len);
        return;
    }

    const char *meta = rec->metadata ? rec->metadata : "null";
    int rc = fprintf(
        svc->jsonl_append_fp,
        "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":%llu,\"len\":%zu}\n",
        rec->record_id, data_esc, meta, (unsigned long long)rec->created_at, rec->len);
    if (rc < 0) {
        SVC_LOG_WARN("mem_d persist: append write failed (errno=%d)", errno);
    } else {
        if (fflush(svc->jsonl_append_fp) != 0) {
            SVC_LOG_WARN("mem_d persist: fflush failed (errno=%d)", errno);
        }
    }
    AIRY_FREE(data_esc);
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

    /* P2: write to a temp file in the same directory, fsync, then rename over
     * the target so a crash mid-rewrite never truncates the whole memory
     * store. */
    char tmppath[4096];
    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", svc->jsonl_path) >=
        (int)sizeof(tmppath)) {
        SVC_LOG_WARN("mem_d persist: rewrite tmp path too long (path=%s)", svc->jsonl_path);
        return;
    }

    FILE *f = fopen(tmppath, "w");
    if (!f) {
        SVC_LOG_WARN("mem_d persist: open rewrite tmp failed (path=%s errno=%d)", tmppath, errno);
        return;
    }

    for (size_t i = 0; i < svc->record_count; i++) {
        const mem_record_entry_t *rec = &svc->records[i];
        if (!rec->record_id || !rec->data)
            continue;
        char *data_esc = NULL;
        mem_json_escape((const char *)rec->data, rec->len, &data_esc);
        if (!data_esc)
            continue;
        const char *meta = rec->metadata ? rec->metadata : "null";
        int wrc = fprintf(f,
                          "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":"
                          "%llu,\"len\":%zu}\n",
                          rec->record_id, data_esc, meta, (unsigned long long)rec->created_at,
                          rec->len);
        AIRY_FREE(data_esc);
        if (wrc < 0) {
            SVC_LOG_WARN("mem_d persist: rewrite write failed (errno=%d)", errno);
            fclose(f);
            remove(tmppath);
            return;
        }
    }

    int failed = (fflush(f) != 0);
#ifndef _WIN32
    if (!failed) {
        int fd = fileno(f);
        if (fd >= 0 && fsync(fd) != 0)
            failed = 1;
    }
#else
    if (!failed) {
        int fd = _fileno(f);
        if (fd >= 0 && _commit(fd) != 0)
            failed = 1;
    }
#endif
    if (fclose(f) != 0)
        failed = 1;

    if (failed) {
        SVC_LOG_WARN("mem_d persist: rewrite flush failed (errno=%d)", errno);
        remove(tmppath);
        return;
    }

#ifndef _WIN32
    if (rename(tmppath, svc->jsonl_path) != 0) {
#else
    if (!MoveFileExA(tmppath, svc->jsonl_path, MOVEFILE_REPLACE_EXISTING)) {
#endif
        SVC_LOG_WARN("mem_d persist: rewrite rename failed (errno=%d)", errno);
        remove(tmppath);
        return;
    }
}

/* ── Load ────────────────────────────────────────────────────────────── */

void mem_persist_load_existing(mem_service_t *svc)
{
    if (!svc || !svc->jsonl_path)
        return;

    FILE *f = fopen(svc->jsonl_path, "r");
    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        return;
    }
    rewind(f);

    char *content = (char *)AIRY_MALLOC((size_t)fsize + 1);
    if (!content) {
        SVC_LOG_WARN("mem_d persist: load alloc failed (%ld bytes)", fsize);
        fclose(f);
        return;
    }
    size_t read_len = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[read_len] = '\0';

    size_t loaded = 0;
    char *line_start = content;
    for (size_t i = 0; i <= read_len; i++) {
        if (i == read_len || content[i] == '\n') {
            content[i] = '\0';
            size_t llen = strlen(line_start);
            while (llen > 0 && line_start[llen - 1] == '\r') {
                line_start[--llen] = '\0';
            }
            if (llen > 0) {
                cJSON *root = cJSON_Parse(line_start);
                if (!root) {
                    SVC_LOG_WARN("mem_d persist: skip malformed jsonl line");
                } else {
                    cJSON *rid = cJSON_GetObjectItem(root, "record_id");
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    cJSON *meta = cJSON_GetObjectItem(root, "metadata");
                    cJSON *cat = cJSON_GetObjectItem(root, "created_at");
                    cJSON *len_item = cJSON_GetObjectItem(root, "len");

                    if (cJSON_IsString(rid) && cJSON_IsString(data) &&
                        svc->record_count < svc->max_records) {
                        size_t idx = svc->record_count;
                        mem_record_entry_t *rec = &svc->records[idx];
                        const char *data_str = data->valuestring;
                        size_t data_len = (len_item && cJSON_IsNumber(len_item)) ?
                                              (size_t)len_item->valuedouble :
                                              strlen(data_str);
                        if (data_len == 0)
                            data_len = strlen(data_str);

                        rec->record_id = AIRY_STRDUP(rid->valuestring);
                        rec->data = AIRY_MALLOC(data_len + 1);
                        if (rec->record_id && rec->data) {
                            __builtin_memcpy(rec->data, data_str, data_len);
                            ((char *)rec->data)[data_len] = '\0';
                            rec->len = data_len;
                            if (meta && !cJSON_IsNull(meta)) {
                                char *meta_str = cJSON_PrintUnformatted(meta);
                                if (meta_str) {
                                    rec->metadata = AIRY_STRDUP(meta_str);
                                    cJSON_free(meta_str);
                                }
                            }
                            rec->score = 0.0f;
                            rec->created_at = (cat && cJSON_IsNumber(cat)) ?
                                                  (uint64_t)cat->valuedouble :
                                                  (uint64_t)time(NULL);
                            if (mem_ht_insert(&svc->record_index, rec->record_id, idx) ==
                                AIRY_SUCCESS) {
                                mem_record_build_vector(svc, rec);
                                svc->record_count++;
                                loaded++;
                            } else {
                                AIRY_FREE(rec->record_id);
                                AIRY_FREE(rec->data);
                                AIRY_FREE(rec->metadata);
                                rec->record_id = NULL;
                                rec->data = NULL;
                                rec->metadata = NULL;
                            }
                        } else {
                            AIRY_FREE(rec->record_id);
                            AIRY_FREE(rec->data);
                            rec->record_id = NULL;
                            rec->data = NULL;
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            line_start = content + i + 1;
        }
    }

    AIRY_FREE(content);
    if (loaded > 0)
        SVC_LOG_INFO("mem_d persist: loaded %zu records from %s", loaded, svc->jsonl_path);
}
