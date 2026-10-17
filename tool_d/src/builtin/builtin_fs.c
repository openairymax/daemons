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

#include <errno.h>

#include "builtin/builtin_fs_internal.h"
#include "io.h"

/* 读取沙箱内已确认路径的全部内容；失败时把面向调用方的错误写入 res。
 * 回执与错误一律回显已解析的绝对路径 resolved（工具路径锚点契约，缺陷 #4），
 * 使调用方能据此判断相对路径的解析基准。写文件统一委托 commons 的
 * airy_io_write_file（同目录 .tmp + fsync + rename 原子替换），本域不再
 * 私有复刻该机制。 */
static int fs_read_whole(const char *resolved, tool_result_t *res, char **out, int *out_truncated)
{
    FILE *fp = fopen(resolved, "rb");
    if (!fp) {
        int rc = (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
        return btool_fail(res, rc, "Cannot open file '%s': %s", resolved, strerror(errno));
    }
    int truncated = 0;
    char *content = builtin_read_all(fp, &truncated);
    fclose(fp);
    if (!content)
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Failed to read file (I/O error)");
    *out = content;
    if (out_truncated)
        *out_truncated = truncated;
    return AIRY_OK;
}

int fs_read_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: path");
    char resolved[4096];
    int rc = builtin_fs_confine(path->valuestring, 0, resolved, sizeof(resolved), res);
    if (rc != AIRY_OK)
        return rc;
    int truncated = 0;
    char *content = NULL;
    rc = fs_read_whole(resolved, res, &content, &truncated);
    if (rc != AIRY_OK)
        return rc;
    if (truncated) {
        const char mark[] = "[output truncated at 1MB]";
        __builtin_memcpy(content + BUILTIN_OUTPUT_CAP - sizeof(mark), mark, sizeof(mark));
    }
    return btool_okout(res, content);
}

/* 写前父目录预检（N3）：resolved 已锚定为绝对路径。create=1 时递归创建
 * 父链；create=0 时只校验父目录存在且为目录，使失败点在进入写系统调用
 * 之前显式暴露（缺失给 NOT_FOUND 与可操作提示，非目录给 IO）。默认开启，
 * 可由 fs_write 的 create_dirs=0 关闭以防误建。 */
static int fs_write_parents(const char *resolved, int create, tool_result_t *res)
{
    char dir[4096];
    size_t n = strlen(resolved);
    if (n == 0 || n >= sizeof(dir))
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Invalid write path");
    __builtin_memcpy(dir, resolved, n + 1);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *bsep = strrchr(dir, '\\');
    if (!sep || (bsep && bsep > sep))
        sep = bsep;
#endif
    if (!sep || sep == dir)
        return AIRY_OK; /* 无父目录分隔符，或直接位于根下，无需创建 */
    *sep = '\0';
    if (create) {
        if (airy_io_mkdir_p(dir, 0755) != 0)
            return btool_fail(res, AIRY_ERR_IO, "Cannot create directory '%s'", dir);
        return AIRY_OK;
    }
    struct stat st;
    if (stat(dir, &st) != 0)
        return btool_fail(res, AIRY_ERR_NOT_FOUND,
                          "Parent directory '%s' does not exist (create_dirs=false)", dir);
    if (!S_ISDIR(st.st_mode))
        return btool_fail(res, AIRY_ERR_IO, "Parent path '%s' is not a directory", dir);
    return AIRY_OK;
}

int fs_write_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: path");
    if (!cJSON_IsString(content) || !content->valuestring)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: content");
    int create_dirs = 1;
    cJSON *cd = cJSON_GetObjectItem(root, "create_dirs");
    if (cJSON_IsBool(cd))
        create_dirs = cJSON_IsTrue(cd);
    size_t clen = strlen(content->valuestring);
    char resolved[4096];
    int rc = builtin_fs_confine(path->valuestring, 1, resolved, sizeof(resolved), res);
    if (rc != AIRY_OK)
        return rc;
    rc = fs_write_parents(resolved, create_dirs, res);
    if (rc != AIRY_OK)
        return rc;
    if (airy_io_write_file(resolved, content->valuestring, clen) != 0)
        return btool_fail(res, AIRY_ERR_IO, "Cannot write file '%s': %s", resolved,
                          strerror(errno));
    return btool_okf(res, "Written %zu bytes to %s (atomic)", clen, resolved);
}

/* ============================================================================
 * fs_edit: precise string-replacement editing (modeled on Codex apply_patch /
 * Claude Code Edit)
 *   params: path (required), old (required, string to replace), new (required),
 *   count (optional 1, number of replacements)
 *   output: replacement summary; a missing old match returns a clear error
 *   (for the LLM to adjust)
 * ============================================================================ */

int fs_edit_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *old = cJSON_GetObjectItem(root, "old");
    cJSON *new = cJSON_GetObjectItem(root, "new");
    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0] ||
        !cJSON_IsString(old) || !old->valuestring || !old->valuestring[0] || !cJSON_IsString(new) ||
        !new->valuestring)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Missing required string parameter: path/old/new");
    int max_rep = (cJSON_IsNumber(cnt) && cnt->valueint > 0) ? cnt->valueint : 1;

    char resolved[4096];
    int rc = builtin_fs_confine(path->valuestring, 1, resolved, sizeof(resolved), res);
    if (rc != AIRY_OK)
        return rc;

    char *content = NULL;
    rc = fs_read_whole(resolved, res, &content, NULL);
    if (rc != AIRY_OK)
        return rc;

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
        AIRY_FREE(content);
        return btool_soft(res, "String not found in '%s': %s", resolved, old->valuestring);
    }
    int reps = (total < max_rep) ? total : max_rep;

    size_t new_size = content_len - (size_t)reps * olen + (size_t)reps * nlen;
    char *buf = (char *)AIRY_MALLOC(new_size + 1);
    if (!buf) {
        AIRY_FREE(content);
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Out of memory");
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

    if (airy_io_write_file(resolved, buf, w) != 0) {
        AIRY_FREE(buf);
        return btool_fail(res, AIRY_ERR_IO, "Cannot write file '%s': %s", resolved,
                          strerror(errno));
    }
    AIRY_FREE(buf);
    return btool_okf(res,
                     "Replaced %d occurrence(s) of %zu-byte string in '%s' "
                     "(total matches: %d, %zu bytes written, atomic)",
                     reps, olen, resolved, total, w);
}
