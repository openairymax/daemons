// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_fs_dir.c
 * @brief Built-in tool file domain: fs_list / fs_delete directory
 *        management tool implementations (functional domain after
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
