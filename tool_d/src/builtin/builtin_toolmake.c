/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_toolmake.c
 * @brief 会话内自制工具（tool_create）：脚本落盘、登记与 ACL 授权。
 *
 * 三条防线：名字白名单（防路径穿越与内置工具抢占）、脚本必须以 shebang
 * 开头（解释器显式声明，不做猜测）、只能覆盖本机制自己在同一路径创建的
 * 脚本（部署期声明的外部工具与 builtin 一律拒绝）。失败一律 fail-closed，
 * 不写半成品：先校验再落盘，落盘失败不登记。
 */

#include "builtin/builtin_toolmake_internal.h"

#include "builtin/builtin_result_internal.h"
#include "platform_misc.h"
#include "platform_paths.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#define TOOLMAKE_NAME_MAX 32
#define TOOLMAKE_SCRIPT_MAX (64U << 10)
#define TOOLMAKE_PARAM_MAX 16

static tool_service_t *g_svc;

void toolmake_attach(tool_service_t *svc)
{
    g_svc = svc;
}

static int make_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > TOOLMAKE_NAME_MAX)
        return 0;
    if (strcmp(name, "tool_create") == 0)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return 0;
    }
    return 1;
}

static int make_script_path(const char *name, char *path, size_t cap)
{
    const char *data = airy_data_dir();
    if (!data || data[0] == '\0')
        return AIRY_ERR_STATE_ERROR;
    char dir[512];
    int n = snprintf(dir, sizeof(dir), "%s/scripts", data);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return AIRY_ERR_INVALID_PARAM;
    if (airy_mkdir_p(dir) != 0)
        return AIRY_ERR_UNKNOWN;
    n = snprintf(path, cap, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= cap)
        return AIRY_ERR_INVALID_PARAM;
    return 0;
}

static int make_schema_ok(const char *type)
{
    return !type || strcmp(type, "string") == 0 || strcmp(type, "integer") == 0 ||
           strcmp(type, "number") == 0 || strcmp(type, "boolean") == 0;
}

/* params: [{"name":"q","type":"string","required":true}, ...] -> 元数据参数表。
 * type 缺省 string（脚本最常用形态），required 缺省为真（脚本参数通常必填）。
 * 返回参数个数，失败返回负值。调用方用 make_par_free 释放。 */
static int make_params(const cJSON *params, tool_param_t **out)
{
    *out = NULL;
    if (!params)
        return 0;
    if (!cJSON_IsArray(params))
        return AIRY_ERR_INVALID_PARAM;
    int n = cJSON_GetArraySize(params);
    if (n <= 0 || n > TOOLMAKE_PARAM_MAX)
        return AIRY_ERR_INVALID_PARAM;
    tool_param_t *arr = AIRY_CALLOC((size_t)n, sizeof(tool_param_t));
    if (!arr)
        return AIRY_ERR_OUT_OF_MEMORY;
    int i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, params)
    {
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *jr = cJSON_GetObjectItemCaseSensitive(item, "required");
        if (!cJSON_IsString(jn) || !jn->valuestring || jn->valuestring[0] == '\0' ||
            !make_schema_ok(cJSON_IsString(jt) ? jt->valuestring : NULL)) {
            make_par_free(arr, i);
            return AIRY_ERR_INVALID_PARAM;
        }
        const char *type = cJSON_IsString(jt) ? jt->valuestring : "string";
        char schema[48];
        snprintf(schema, sizeof(schema), "{\"type\":\"%s\"}", type);
        arr[i].name = AIRY_STRDUP(jn->valuestring);
        arr[i].schema = AIRY_STRDUP(schema);
        arr[i].required = cJSON_IsBool(jr) ? (cJSON_IsTrue(jr) ? 1 : 0) : 1;
        if (!arr[i].name || !arr[i].schema) {
            make_par_free(arr, i + 1);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        ++i;
    }
    *out = arr;
    return i;
}

void make_par_free(tool_param_t *arr, int n)
{
    if (!arr)
        return;
    for (int i = 0; i < n; ++i) {
        AIRY_FREE((char *)arr[i].name);
        AIRY_FREE((char *)arr[i].schema);
    }
    AIRY_FREE(arr);
}

/* 落盘：先写临时文件再 rename，避免执行面读到半截脚本。 */
static int make_write_script(const char *path, const char *script, size_t len)
{
    char tmp[576];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return AIRY_ERR_INVALID_PARAM;
    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return AIRY_ERR_UNKNOWN;
    size_t written = fwrite(script, 1, len, fp);
    int flush_rc = fflush(fp);
    fclose(fp);
    if (written != len || flush_rc != 0) {
        remove(tmp);
        return AIRY_ERR_UNKNOWN;
    }
#ifndef _WIN32
    if (chmod(tmp, 0700) != 0) {
        remove(tmp);
        return AIRY_ERR_UNKNOWN;
    }
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return AIRY_ERR_UNKNOWN;
    }
    return 0;
}

/* 参数面校验：三个必填字符串 + 名字白名单 + 脚本长度与 shebang 形态。
 * 全部在落盘前完成，任何一项不合法都不产生副作用。 */
static int make_args(cJSON *root, tool_result_t *res, const char **name, const char **desc,
                     const char **script)
{
    int rc = builtin_str_param(root, "name", name, res);
    if (rc != AIRY_OK)
        return rc;
    rc = builtin_str_param(root, "description", desc, res);
    if (rc != AIRY_OK)
        return rc;
    rc = builtin_str_param(root, "script", script, res);
    if (rc != AIRY_OK)
        return rc;
    if (!make_name_ok(*name))
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "tool_create: name must match [A-Za-z0-9_]{1,32} and must not be "
                          "\"tool_create\"");
    size_t slen = strlen(*script);
    if (slen > TOOLMAKE_SCRIPT_MAX)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "tool_create: script exceeds %u bytes",
                          TOOLMAKE_SCRIPT_MAX);
    if (slen < 3 || (*script)[0] != '#' || (*script)[1] != '!')
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "tool_create: script must start with a shebang (e.g. #!/bin/sh)");
    return AIRY_OK;
}

/* 覆盖判定（先于落盘）：只有本机制在同一路径创建的脚本可重定义；
 * 部署期声明的外部工具与内置工具一律拒绝，避免脚本抢占能力格。 */
static int make_owner(const char *name, const char *path, tool_result_t *res, int *updated)
{
    *updated = 0;
    int owner = toolmake_svc_owner(g_svc, name, path);
    if (owner < 0)
        return btool_fail(res, AIRY_ERR_PERMISSION_DENIED,
                          "tool_create: '%s' is already registered by the deployment; "
                          "choose another name",
                          name);
    *updated = owner;
    return AIRY_OK;
}

/* 登记：注册或就地替换，并使能 ACL。登记失败即回滚脚本，不留半成品。 */
static int make_install(const char *name, const char *desc, tool_param_t *params, int pn,
                        const char *path, int updated)
{
    int rc = toolmake_svc_install(g_svc, name, desc, params, pn, path, updated);
    if (rc != AIRY_OK)
        remove(path);
    return rc;
}

/* 回执：把落点与形态回给模型，使其下一轮可直接按名调用。 */
static int make_report(const char *name, const char *desc, const char *path, int updated, int pn,
                       tool_result_t *res)
{
    cJSON *out = cJSON_CreateObject();
    if (!out)
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "tool_create: out of memory");
    cJSON_AddStringToObject(out, "name", name);
    cJSON_AddStringToObject(out, "description", desc);
    cJSON_AddStringToObject(out, "path", path);
    cJSON_AddBoolToObject(out, "updated", updated ? 1 : 0);
    cJSON_AddNumberToObject(out, "param_count", pn);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!text)
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "tool_create: out of memory");
    SVC_LOG_INFO("tool_create: %s %s (%d params)", updated ? "redefined" : "created", name, pn);
    return btool_okout(res, text);
}

int toolmake_create(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
    if (!g_svc)
        return btool_fail(res, AIRY_ERR_STATE_ERROR,
                          "tool_create: tool service is not attached");

    cJSON *root = cJSON_Parse(params_json && params_json[0] ? params_json : "{}");
    if (!root)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "tool_create: arguments are not valid JSON");

    const char *name = NULL;
    const char *desc = NULL;
    const char *script = NULL;
    tool_param_t *params = NULL;
    int pn = 0;
    int updated = 0;
    char path[512];

    int rc = make_args(root, res, &name, &desc, &script);
    if (rc != AIRY_OK)
        goto done;
    pn = make_params(cJSON_GetObjectItemCaseSensitive(root, "params"), &params);
    if (pn < 0) {
        rc = btool_fail(res, AIRY_ERR_INVALID_PARAM,
                        "tool_create: params must be an array of {name,type?,required?}, "
                        "1..%d entries, type in string|integer|number|boolean",
                        TOOLMAKE_PARAM_MAX);
        goto done;
    }
    rc = make_script_path(name, path, sizeof(path));
    if (rc != AIRY_OK) {
        rc = btool_fail(res, rc, "tool_create: cannot prepare script directory");
        goto done;
    }
    rc = make_owner(name, path, res, &updated);
    if (rc != AIRY_OK)
        goto done;
    rc = make_write_script(path, script, strlen(script));
    if (rc != AIRY_OK) {
        rc = btool_fail(res, rc, "tool_create: failed to write script to %s", path);
        goto done;
    }
    rc = make_install(name, desc, params, pn, path, updated);
    if (rc != AIRY_OK) {
        rc = btool_fail(res, rc, "tool_create: failed to register '%s'", name);
        goto done;
    }
    rc = make_report(name, desc, path, updated, pn, res);

done:
    make_par_free(params, pn > 0 ? pn : 0);
    cJSON_Delete(root);
    return rc;
}
