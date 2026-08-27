// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs_grep.c
 * @brief Built-in tool file domain: fs_grep regex content search tool
 *        implementation (functional domain after builtin_fs.c split).
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
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#define S_ISDIR(m) (((m)&_S_IFDIR) != 0)
#define S_ISREG(m) (((m)&_S_IFREG) != 0)
#endif

#include "tool_builtin_internal.h"

/* ============================================================================
 * fs_grep: regex content search (modeled on Atom Code GrepTool / Claude Code rg guidance)
 *   params: pattern (required, POSIX ERE), path (optional "."), max_results (optional 200)
 *   output: relpath:lineno:line newline-separated; skips noise dirs like
 *   .git/.venv and binaries
 * ============================================================================ */

#define BUILTIN_GREP_MAX 200

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
