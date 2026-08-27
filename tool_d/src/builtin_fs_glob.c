// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs_glob.c
 * @brief Built-in tool file domain: fs_glob recursive wildcard file
 *        listing tool implementation (functional domain after
 *        builtin_fs.c split).
 */

#include "airy_memory.h"
#include "error.h"

#include "airy_dirent.h"
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
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#define S_ISDIR(m) (((m)&_S_IFDIR) != 0)
#endif

#include "tool_builtin_internal.h"

/* ============================================================================
 * fs_glob: recursive wildcard file listing (modeled on Atom Code GlobTool /
 * Codex find guidance)
 *   params: pattern (required, supports * ? and ** segments), base (optional, default ".")
 *   output: newline-separated matching relative paths; capped at BUILTIN_GLOB_MAX entries
 * ============================================================================ */

#define BUILTIN_GLOB_MAX 2000

int builtin_glob_seg_match(const char *pat, const char *str)
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
