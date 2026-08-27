// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs.c
 * @brief Built-in tool file domain: fs_read / fs_write / fs_edit
 *        file read/write tool implementations (functional domain after
 *        builtin_fs.c split; directory listing/deletion live in
 *        builtin_fs_dir.c, glob in builtin_fs_glob.c, grep in
 *        builtin_fs_grep.c).
 */

#include "airy_memory.h"
#include "error.h"

#include "builtin.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

#include "tool_builtin_internal.h"

/* ============================================================================
 * 原子写（t11-01）：写同目录临时文件 + fsync + rename 替换。
 * 直接 fopen(path,"wb") 覆盖写在中途失败/崩溃时会留下半写文件；
 * 改为先写 "<path>.tmp"（同目录保证同卷 rename 原子性）再原子替换，
 * 读取方永远只能看到旧内容或新内容，不会看到损坏的中间态。
 * 返回 0 成功；非 0 失败（errno 描述原因）。
 * ============================================================================ */
static int fs_atomic_write(const char *path, const char *buf, size_t len)
{
    char tmppath[4096];
    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", path) >= (int)sizeof(tmppath))
        return -1;

    FILE *wfp = fopen(tmppath, "wb");
    if (!wfp)
        return -1;

    int failed = 0;
    size_t wr = fwrite(buf, 1, len, wfp);
    if (wr != len)
        failed = 1;
    if (!failed && fflush(wfp) != 0)
        failed = 1;
#ifndef _WIN32
    if (!failed) {
        int fd = fileno(wfp);
        if (fd >= 0 && fsync(fd) != 0)
            failed = 1;
    }
#else
    if (!failed) {
        int fd = _fileno(wfp);
        if (fd >= 0 && _commit(fd) != 0)
            failed = 1;
    }
#endif
    if (fclose(wfp) != 0)
        failed = 1;

    if (failed) {
        remove(tmppath);
        return -1;
    }

#ifndef _WIN32
    if (rename(tmppath, path) != 0) {
        remove(tmppath);
        return -1;
    }
#else
    /* MoveFileExA 可原子替换已存在的目标（C 标准 rename 在目标存在时会失败） */
    if (!MoveFileExA(tmppath, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmppath);
        return -1;
    }
#endif
    return 0;
}

int fs_read_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: path");
        return AIRY_ERR_INVALID_PARAM;
    }
    FILE *fp = fopen(path->valuestring, "rb");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open file '%s': %s", path->valuestring, strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    int truncated = 0;
    char *content = builtin_read_all(fp, &truncated);
    fclose(fp);
    if (!content) {
        res->error = AIRY_STRDUP("Failed to read file (I/O error)");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (truncated) {

        const char mark[] = "[output truncated at 1MB]";
        __builtin_memcpy(content + BUILTIN_OUTPUT_CAP - sizeof(mark), mark, sizeof(mark));
    }
    res->output = content;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

int fs_write_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: path");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!cJSON_IsString(content) || !content->valuestring) {
        res->error = AIRY_STRDUP("Missing string parameter: content");
        return AIRY_ERR_INVALID_PARAM;
    }
    size_t clen = strlen(content->valuestring);
    if (fs_atomic_write(path->valuestring, content->valuestring, clen) != 0) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot write file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }
    char ok[512];
    snprintf(ok, sizeof(ok), "Written %zu bytes to %s (atomic)", clen, path->valuestring);
    res->output = AIRY_STRDUP(ok);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * fs_edit: precise string-replacement editing (modeled on Codex apply_patch /
 * Claude Code Edit)
 *   params: path (required), old (required, string to replace), new (required),
 *   count (optional 1, number of replacements)
 *   output: replacement summary; a missing old match returns a clear error
 *   (for the LLM to adjust)
 * ============================================================================ */

int fs_edit_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *old = cJSON_GetObjectItem(root, "old");
    cJSON *new = cJSON_GetObjectItem(root, "new");
    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0] ||
        !cJSON_IsString(old) || !old->valuestring || !old->valuestring[0] || !cJSON_IsString(new) ||
        !new->valuestring) {
        res->error = AIRY_STRDUP("Missing required string parameter: path/old/new");
        return AIRY_ERR_INVALID_PARAM;
    }
    int max_rep = (cJSON_IsNumber(cnt) && cnt->valueint > 0) ? cnt->valueint : 1;

    FILE *fp = fopen(path->valuestring, "rb");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open file '%s': %s", path->valuestring, strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    int truncated = 0;
    char *content = builtin_read_all(fp, &truncated);
    fclose(fp);
    if (!content) {
        res->error = AIRY_STRDUP("Failed to read file (I/O error)");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t olen = strlen(old->valuestring);
    size_t nlen = strlen(new->valuestring);
    size_t content_len = strlen(content);
    int total = 0;
    {
        const char *p = content;
        while ((p = strstr(p, old->valuestring)) != NULL) {
            total++;
            p += olen;
        }
    }
    if (total == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "String not found in '%s': %s", path->valuestring,
                 old->valuestring);
        res->error = AIRY_STRDUP(msg);
        AIRY_FREE(content);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    int reps = (total < max_rep) ? total : max_rep;

    size_t new_size = content_len - (size_t)reps * olen + (size_t)reps * nlen;
    char *buf = (char *)AIRY_MALLOC(new_size + 1);
    if (!buf) {
        AIRY_FREE(content);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t w = 0, done = 0;
    const char *p = content;
    const char *cur = content;
    while (done < (size_t)reps && (p = strstr(cur, old->valuestring)) != NULL) {
        __builtin_memcpy(buf + w, cur, (size_t)(p - cur));
        w += (size_t)(p - cur);
        __builtin_memcpy(buf + w, new->valuestring, nlen);
        w += nlen;
        cur = p + olen;
        done++;
    }
    if (w < new_size) {
        __builtin_memcpy(buf + w, cur, new_size - w);
        w = new_size;
    }
    buf[w] = '\0';
    AIRY_FREE(content);

    if (fs_atomic_write(path->valuestring, buf, w) != 0) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot write file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        AIRY_FREE(buf);
        return AIRY_ERR_IO;
    }
    char ok[512];
    snprintf(ok, sizeof(ok),
             "Replaced %d occurrence(s) of %zu-byte string in '%s' "
             "(total matches: %d, %zu bytes written, atomic)",
             reps, olen, path->valuestring, total, w);
    AIRY_FREE(buf);
    res->output = AIRY_STRDUP(ok);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}
