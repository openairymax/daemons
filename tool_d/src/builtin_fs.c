// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs.c
 * @brief Built-in tool file domain: fs_read / fs_write / fs_list /
 *        fs_glob / fs_grep / fs_edit filesystem-operation tool impls.
 */

#include "airy_memory.h"
#include "error.h"

#include "airy_dirent.h"
#include "airy_regex.h"
#include "builtin.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#define lstat _stat
#define unlink _unlink
#define rmdir _rmdir
#define S_ISDIR(m) (((m)&_S_IFDIR) != 0)
#define S_ISREG(m) (((m)&_S_IFREG) != 0)
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 206 /* MSVC errno.h lacks ENAMETOOLONG */
#endif
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

int fs_list_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    const char *dir = NULL;
    if (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) {
        dir = path->valuestring;
    }
    DIR *d = opendir(dir ? dir : ".");
    if (!d) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open directory '%s': %s", dir ? dir : ".",
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    cJSON *arr = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", ent->d_name);
#ifdef DT_DIR
        cJSON_AddStringToObject(item, "type", ent->d_type == DT_DIR ? "dir" : "file");
#else
        /* Windows: no d_type; classify via stat. */
        char full[AIRY_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir ? dir : ".", ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0)
            cJSON_AddStringToObject(item, "type", S_ISDIR(st.st_mode) ? "dir" : "file");
#endif
        cJSON_AddItemToArray(arr, item);
    }
    closedir(d);
    res->output = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!res->output) {
        res->error = AIRY_STRDUP("Failed to serialize directory listing");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * fs_glob: recursive wildcard file listing (modeled on Atom Code GlobTool /
 * Codex find guidance)
 *   params: pattern (required, supports * ? and ** segments), base (optional, default ".")
 *   output: newline-separated matching relative paths; capped at BUILTIN_GLOB_MAX entries
 * ============================================================================ */

#define BUILTIN_GLOB_MAX 2000
#define BUILTIN_GREP_MAX 200

static int builtin_glob_seg_match(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*')
                pat++;
            if (!*pat)
                return 1;
            for (const char *s = str; *s; s++) {
                if (builtin_glob_seg_match(pat, s))
                    return 1;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*str)
                return 0;
            pat++;
            str++;
        } else {
            if (*pat != *str)
                return 0;
            pat++;
            str++;
        }
    }
    return *str == '\0';
}

static void builtin_glob_impl(const char *base, const char **segs, size_t n, size_t i, char *path,
                              size_t path_len, size_t path_cap, char *out, size_t out_cap,
                              size_t *out_len, size_t *count, size_t max)
{
    if (*count >= max || *out_len >= out_cap - 1)
        return;

    char full[AIRY_PATH_MAX];
    if (path_len > 0)
        snprintf(full, sizeof(full), "%s/%s", base, path);
    else
        snprintf(full, sizeof(full), "%s", base);

    if (i == n) {

        if (path_len > 0 && path_len + 2 <= out_cap - *out_len) {
            __builtin_memcpy(out + *out_len, path, path_len);
            *out_len += path_len;
            out[(*out_len)++] = '\n';
            out[*out_len] = '\0';
            (*count)++;
        }
        return;
    }

    const char *seg = segs[i];
    const int is_recursive = (strcmp(seg, "**") == 0);

    if (is_recursive) {

        builtin_glob_impl(base, segs, n, i + 1, path, path_len, path_cap, out, out_cap, out_len,
                          count, max);

        DIR *d = opendir(full);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && *count < max) {
                const char *nm = ent->d_name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                    continue;
#ifdef DT_DIR
                int is_dir = (ent->d_type == DT_DIR);
#else
                struct stat st;
                char probe[AIRY_PATH_MAX];
                snprintf(probe, sizeof(probe), "%s/%s", full, nm);
                int is_dir = (stat(probe, &st) == 0 && S_ISDIR(st.st_mode));
#endif
                if (!is_dir)
                    continue;
                size_t need = (path_len ? path_len + 1 : 0) + strlen(nm);
                if (need + 1 >= path_cap)
                    continue;
                char *p = path + path_len;
                if (path_len)
                    *p++ = '/';
                __builtin_memcpy(p, nm, strlen(nm) + 1);
                builtin_glob_impl(base, segs, n, i, path,
                                  path_len + (path_len ? 1 : 0) + strlen(nm), path_cap, out,
                                  out_cap, out_len, count, max);
                if (path_len)
                    path[path_len] = '\0';
            }
            closedir(d);
        }
        return;
    }

    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (!builtin_glob_seg_match(seg, nm))
            continue;
#ifdef DT_DIR
        int is_dir = (ent->d_type == DT_DIR);
#else
        struct stat st;
        char probe[AIRY_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/%s", full, nm);
        int is_dir = (stat(probe, &st) == 0 && S_ISDIR(st.st_mode));
#endif

        size_t need = (path_len ? path_len + 1 : 0) + strlen(nm);
        if (need + 1 >= path_cap)
            continue;
        char *p = path + path_len;
        if (path_len)
            *p++ = '/';
        __builtin_memcpy(p, nm, strlen(nm) + 1);
        if (i + 1 == n) {

            builtin_glob_impl(base, segs, n, i + 1, path,
                              path_len + (path_len ? 1 : 0) + strlen(nm), path_cap, out, out_cap,
                              out_len, count, max);
        } else if (is_dir) {
            builtin_glob_impl(base, segs, n, i + 1, path,
                              path_len + (path_len ? 1 : 0) + strlen(nm), path_cap, out, out_cap,
                              out_len, count, max);
        }
        if (path_len)
            path[path_len] = '\0';
    }
    closedir(d);
}

int fs_glob_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *pat = cJSON_GetObjectItem(root, "pattern");
    if (!cJSON_IsString(pat) || !pat->valuestring || !pat->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *base = cJSON_GetObjectItem(root, "base");
    const char *base_dir = (cJSON_IsString(base) && base->valuestring && base->valuestring[0]) ?
                               base->valuestring :
                               ".";

    const char *segs[64];
    size_t nsegs = 0;
    const char *s = pat->valuestring;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (*e && *e != '/')
            e++;
        if (nsegs >= 64) {
            res->error = AIRY_STRDUP("pattern too deep (max 64 segments)");
            return AIRY_ERR_INVALID_PARAM;
        }
        char *seg = (char *)AIRY_MALLOC((size_t)(e - s) + 1);
        if (!seg) {
            for (size_t k = 0; k < nsegs; k++)
                AIRY_FREE((void *)segs[k]);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(seg, s, (size_t)(e - s));
        seg[e - s] = '\0';
        segs[nsegs++] = seg;
        s = e;
    }
    if (nsegs == 0) {
        res->error = AIRY_STRDUP("pattern is empty after splitting");
        return AIRY_ERR_INVALID_PARAM;
    }

    char *out = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    char *path = (char *)AIRY_MALLOC(AIRY_PATH_MAX);
    if (!out || !path) {
        for (size_t k = 0; k < nsegs; k++)
            AIRY_FREE((void *)segs[k]);
        AIRY_FREE(out);
        AIRY_FREE(path);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = 0, count = 0;
    builtin_glob_impl(base_dir, segs, nsegs, 0, path, 0, AIRY_PATH_MAX, out, BUILTIN_OUTPUT_CAP,
                      &out_len, &count, BUILTIN_GLOB_MAX);

    for (size_t k = 0; k < nsegs; k++)
        AIRY_FREE((void *)segs[k]);
    AIRY_FREE(path);

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No files match pattern '%s' under '%s'", pat->valuestring,
                 base_dir);
        res->error = AIRY_STRDUP(msg);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    if (count >= BUILTIN_GLOB_MAX)
        builtin_append_trunc_mark(out, BUILTIN_OUTPUT_CAP, out_len,
                                  "\n[glob truncated: too many matches]");
    res->output = out;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * fs_grep: regex content search (modeled on Atom Code GrepTool / Claude Code rg guidance)
 *   params: pattern (required, POSIX ERE), path (optional "."), max_results (optional 200)
 *   output: relpath:lineno:line newline-separated; skips noise dirs like
 *   .git/.venv and binaries
 * ============================================================================ */

/* Portable getline replacement: read one line (bytes up to and including
 * '\n' or EOF); returns a NULL-terminated AIRY_MALLOC buffer (caller frees)
 * or NULL on EOF/OOM. *out_len receives the byte count including the
 * newline, so embedded NUL bytes can be detected by the caller. */
static char *builtin_read_line(FILE *fp, size_t *out_len)
{
    size_t cap = 256;
    size_t len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return NULL;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)AIRY_REALLOC(buf, nc);
            if (!nb) {
                AIRY_FREE(buf);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        buf[len++] = (char)c;
        if (c == '\n')
            break;
    }
    if (len == 0) {
        AIRY_FREE(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

static int builtin_grep_dir(const char *base, const char *root, regex_t *re,
                            const char *glob_filter, int max_results, char *out, size_t out_cap,
                            size_t *out_len, int *count, int *done)
{
    DIR *d = opendir(base);
    if (!d)
        return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !*done) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (strcmp(nm, ".git") == 0 || strcmp(nm, "node_modules") == 0 ||
            strcmp(nm, "target") == 0 || strcmp(nm, ".venv") == 0 ||
            strcmp(nm, "__pycache__") == 0 || strcmp(nm, ".airymaxrt") == 0 ||
            strcmp(nm, "build") == 0 || strcmp(nm, "logs") == 0)
            continue;
        char full[AIRY_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", base, nm);
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) {
            builtin_grep_dir(full, root, re, glob_filter, max_results, out, out_cap, out_len, count,
                             done);
            continue;
        }
        if (ent->d_type != DT_REG)
            continue;
#else
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            builtin_grep_dir(full, root, re, glob_filter, max_results, out, out_cap, out_len, count,
                             done);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
#endif
        if (glob_filter && !builtin_glob_seg_match(glob_filter, nm))
            continue;
        if (*count >= max_results)
            break;

        FILE *fp = fopen(full, "rb");
        if (!fp)
            continue;
        long lineno = 0;
        char *line;
        size_t llen;
        while ((line = builtin_read_line(fp, &llen)) != NULL) {
            lineno++;
            if (memchr(line, '\0', llen) != NULL) {
                AIRY_FREE(line);
                break;
            }

            size_t tlen = llen;
            while (tlen > 0 && (line[tlen - 1] == '\n' || line[tlen - 1] == '\r'))
                tlen--;
            if (regexec(re, line, 0, NULL, 0) == 0) {
                if (*count >= max_results) {
                    AIRY_FREE(line);
                    break;
                }

                const char *rel = full;
                if (strncmp(root, full, strlen(root)) == 0 && full[strlen(root)] == '/')
                    rel = full + strlen(root) + 1;
                size_t need = strlen(rel) + 16 + tlen + 2;
                if (*out_len + need >= out_cap) {
                    builtin_append_trunc_mark(out, out_cap, *out_len,
                                              "\n[grep truncated: output cap]");
                    *done = 1;
                    AIRY_FREE(line);
                    break;
                }
                int w = snprintf(out + *out_len, out_cap - *out_len, "%s:%ld:", rel, lineno);
                if (w > 0)
                    *out_len += (size_t)w;
                __builtin_memcpy(out + *out_len, line, tlen);
                *out_len += tlen;
                out[(*out_len)++] = '\n';
                out[*out_len] = '\0';
                (*count)++;
            }
            AIRY_FREE(line);
        }
        fclose(fp);
    }
    closedir(d);
    return 0;
}

int fs_grep_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *pat = cJSON_GetObjectItem(root, "pattern");
    if (!cJSON_IsString(pat) || !pat->valuestring || !pat->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *path = cJSON_GetObjectItem(root, "path");
    const char *dir = (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) ?
                          path->valuestring :
                          ".";
    cJSON *gf = cJSON_GetObjectItem(root, "glob");
    const char *glob_filter = (cJSON_IsString(gf) && gf->valuestring) ? gf->valuestring : NULL;
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    int max_results = (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint : BUILTIN_GREP_MAX;
    if (max_results > 1000)
        max_results = 1000;

    regex_t re;
    if (regcomp(&re, pat->valuestring, REG_EXTENDED | REG_NOSUB) != 0) {
        res->error = AIRY_STRDUP("Invalid regex pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    char *out = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!out) {
        regfree(&re);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = 0;
    int count = 0, done = 0;
    builtin_grep_dir(dir, dir, &re, glob_filter, max_results, out, BUILTIN_OUTPUT_CAP, &out_len,
                     &count, &done);
    regfree(&re);

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No matches for pattern '%s' under '%s'", pat->valuestring, dir);
        res->error = AIRY_STRDUP(msg);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    res->output = out;
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

/* ============================================================================
 * fs_delete - delete a local file or directory.
 * params:
 *   path      (required string): file or directory to delete
 *   recursive (optional bool, 0): delete a non-empty directory tree; without
 *             it a non-empty directory is refused (safety: destructive ops
 *             must be explicit). Directories always require recursive=1;
 *             plain files are removed regardless.
 * output: removal summary; permission/empty checks return clear errors for
 *         the LLM to adjust.
 * ============================================================================ */

/* 递归删除目录树（仅由 fs_delete_tool 的 recursive=1 路径调用）。 */
static int fs_delete_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) {
            closedir(d);
            errno = ENAMETOOLONG;
            return -1;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            closedir(d);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (fs_delete_tree(child) != 0) {
                closedir(d);
                return -1;
            }
        } else if (unlink(child) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

int fs_delete_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *rec = cJSON_GetObjectItem(root, "recursive");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing required string parameter: path");
        return AIRY_ERR_INVALID_PARAM;
    }
    const char *p = path->valuestring;
    if (strcmp(p, "/") == 0 || strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
        res->error = AIRY_STRDUP("Refusing to delete a root or current directory");
        return AIRY_ERR_INVALID_PARAM;
    }
    int recursive = (cJSON_IsBool(rec) && cJSON_IsTrue(rec)) ||
                    (cJSON_IsNumber(rec) && rec->valueint != 0);

    struct stat st;
    if (lstat(p, &st) != 0) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot access '%s': %s", p, strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            /* 非空目录拒绝；空目录允许直接删除（rmdir 语义）。 */
            DIR *d = opendir(p);
            if (!d) {
                char err[512];
                snprintf(err, sizeof(err), "Cannot open directory '%s': %s", p,
                         strerror(errno));
                res->error = AIRY_STRDUP(err);
                return AIRY_ERR_IO;
            }
            int non_empty = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                non_empty = 1;
                break;
            }
            closedir(d);
            if (non_empty) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "Directory '%s' is not empty; set \"recursive\":true to "
                         "delete the whole tree (destructive)",
                         p);
                res->error = AIRY_STRDUP(msg);
                res->success = 0;
                res->exit_code = 1;
                return AIRY_OK;
            }
            if (rmdir(p) == 0) {
                char ok[256];
                snprintf(ok, sizeof(ok), "Removed directory '%s'", p);
                res->output = AIRY_STRDUP(ok);
                res->success = 1;
                res->exit_code = 0;
                return AIRY_OK;
            }
        } else if (fs_delete_tree(p) == 0) {
            char ok[256];
            snprintf(ok, sizeof(ok), "Removed directory tree '%s'", p);
            res->output = AIRY_STRDUP(ok);
            res->success = 1;
            res->exit_code = 0;
            return AIRY_OK;
        }
        char err[512];
        snprintf(err, sizeof(err), "Failed to remove directory '%s': %s", p, strerror(errno));
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }

    if (unlink(p) != 0) {
        char err[512];
        snprintf(err, sizeof(err), "Failed to delete file '%s': %s", p, strerror(errno));
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }
    char ok[256];
    snprintf(ok, sizeof(ok), "Deleted file '%s'", p);
    res->output = AIRY_STRDUP(ok);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}
