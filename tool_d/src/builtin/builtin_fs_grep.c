// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs_grep.c
 * @brief Built-in tool file domain: fs_grep regex content search tool
 *        implementation (functional domain after builtin_fs.c split).
 */

#include "airy_dirent.h"
#include "airy_regex.h"

#include <errno.h>

#include "builtin/builtin_buf_internal.h"
#include "builtin/builtin_fs_internal.h"
#include "builtin/builtin_time_internal.h"

/* ============================================================================
 * fs_grep: regex content search (modeled on Atom Code GrepTool / Claude Code rg guidance)
 *   params: pattern (required, POSIX ERE), path (optional "."), max_results (optional 200)
 *   output: relpath:lineno:line newline-separated; skips noise dirs like
 *   .git/.venv and binaries
 * ============================================================================ */

#define BUILTIN_GREP_MAX 200

/* Read one line of unbounded length via a doubling buffer. *out_len counts
 * bytes including the trailing newline so the caller can detect embedded
 * NULs. Returns NULL when EOF is hit before any byte was read. */
static char *builtin_read_line(FILE *fp, size_t *out_len)
{
    size_t cap = 256, len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return NULL;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nbuf = (char *)AIRY_REALLOC(buf, ncap);
            if (!nbuf) {
                AIRY_FREE(buf);
                return NULL;
            }
            buf = nbuf;
            cap = ncap;
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
    *out_len = len;
    return buf;
}

/* Bounded directory walk: every level checks the wall-clock budget and
 * the depth cap; oversized files are skipped so a single huge log cannot
 * monopolize the executor thread (incident 0.1.16: fs_grep ran 158s over
 * the whole workspace and stalled the DAG pipeline). On budget expiry the
 * partial result is returned with a truncation mark (*timed_out=1). */
typedef struct {
    const char *root;
    const regex_t *re;
    const char *glob;
    int max;
    char *out;
    size_t cap;
    size_t len;
    int count;
    int done;
    int timed_out;
    uint64_t deadline;
} grep_ctx_t;

static void grep_stop(grep_ctx_t *g)
{
    g->done = 1;
    g->timed_out = 1;
}

static int grep_skip(const char *nm)
{
    return strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0 || builtin_scan_noise_dir(nm);
}

/* Emit one matched line as relpath:lineno:line. Returns 1 when the output
 * cap was hit (done set so the whole walk unwinds). */
static int grep_emit(grep_ctx_t *g, const char *full, long lineno, const char *line, size_t tlen)
{
    const char *rel = full;
    size_t rlen = strlen(g->root);
    if (strncmp(g->root, full, rlen) == 0 && full[rlen] == '/')
        rel = full + rlen + 1;
    if (g->len + strlen(rel) + 16 + tlen + 2 >= g->cap) {
        builtin_append_trunc_mark(g->out, g->cap, g->len, "\n[grep truncated: output cap]");
        g->done = 1;
        return 1;
    }
    int w = snprintf(g->out + g->len, g->cap - g->len, "%s:%ld:", rel, lineno);
    if (w > 0)
        g->len += (size_t)w;
    AIRY_MEMCPY(g->out + g->len, line, tlen);
    g->len += tlen;
    g->out[g->len++] = '\n';
    g->out[g->len] = '\0';
    g->count++;
    return 0;
}

/* Scan one regular file line by line; stops on the result cap, the time
 * budget or an embedded NUL. Returns 1 when the walk must unwind. */
static int grep_file(const char *full, grep_ctx_t *g)
{
    FILE *fp = fopen(full, "rb");
    if (!fp)
        return 0;
    long lineno = 0;
    size_t llen = 0;
    char *line;
    while (!g->done && (line = builtin_read_line(fp, &llen)) != NULL) {
        if (builtin_deadline_hit(g->deadline)) {
            grep_stop(g);
            AIRY_FREE(line);
            break;
        }
        lineno++;
        if (memchr(line, '\0', llen) != NULL) {
            AIRY_FREE(line);
            break;
        }
        size_t tlen = llen;
        while (tlen > 0 && (line[tlen - 1] == '\n' || line[tlen - 1] == '\r'))
            tlen--;
        int hit = 0;
        if (regexec(g->re, line, 0, NULL, 0) == 0) {
            hit = (g->count >= g->max) ? 1 : grep_emit(g, full, lineno, line, tlen);
        }
        AIRY_FREE(line);
        if (hit)
            break;
    }
    fclose(fp);
    return g->done;
}

static void grep_walk(const char *base, grep_ctx_t *g, int depth)
{
    if (builtin_deadline_hit(g->deadline) || depth > BUILTIN_SCAN_MAX_DEPTH) {
        grep_stop(g);
        return;
    }
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *ent;
    while (!g->done && (ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (grep_skip(nm))
            continue;
        char full[AIRY_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", base, nm);
        /* Classify via stat only: portable (MSVC dirent has no d_type) and
         * also covers DT_UNKNOWN mounts; the same syscall applies the
         * per-file size cap. */
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            grep_walk(full, g, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || st.st_size > BUILTIN_SCAN_MAX_FILE_BYTES)
            continue;
        if (g->glob && !builtin_glob_seg_match(g->glob, nm))
            continue;
        if (g->count >= g->max)
            break;
        (void)grep_file(full, g);
    }
    closedir(d);
}

int fs_grep_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    const char *pattern = NULL;
    if (builtin_str_param(root, "pattern", &pattern, res) != AIRY_OK)
        return AIRY_ERR_INVALID_PARAM;
    const char *dir = builtin_str_opt(root, "path", ".");
    cJSON *gf = cJSON_GetObjectItem(root, "glob");
    const char *glob_filter = cJSON_IsString(gf) ? gf->valuestring : NULL;
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    int max_results = (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint : BUILTIN_GREP_MAX;
    if (max_results > 1000)
        max_results = 1000;

    char resolved[4096];
    int rc = builtin_fs_confine(dir, 0, resolved, sizeof(resolved), res);
    if (rc != AIRY_OK)
        return rc;

    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Invalid regex pattern");
    char *out = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!out) {
        regfree(&re);
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Out of memory");
    }
    grep_ctx_t g;
    AIRY_MEMSET(&g, 0, sizeof(g));
    g.root = resolved;
    g.re = &re;
    g.glob = glob_filter;
    g.max = max_results;
    g.out = out;
    g.cap = BUILTIN_OUTPUT_CAP;
    g.deadline = builtin_deadline_ms(timeout_ms);
    grep_walk(resolved, &g, 0);
    regfree(&re);

    if (g.count == 0) {
        if (g.timed_out)
            return btool_soft(res, "fs_grep timed out after %ums under '%s' (no matches)",
                              (unsigned)timeout_ms, dir);
        return btool_soft(res, "No matches for pattern '%s' under '%s'", pattern, dir);
    }
    if (g.timed_out) {
        builtin_append_trunc_mark(out, BUILTIN_OUTPUT_CAP, g.len,
                                  "\n[grep truncated: time budget]");
        out[BUILTIN_OUTPUT_CAP - 1] = '\0';
    }
    return btool_okout(res, out);
}
