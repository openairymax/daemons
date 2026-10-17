// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_contract.c
 * @brief 工具契约边车实现：查找策略层绑定的契约表，并把契约投影为
 *        list_tools 输出中的 x-airy 块。
 */

#include "core/tool_contract.h"
#include "sandbox/os_sandbox.h"

#include <cjson/cJSON.h>
#include <string.h>

static const tool_contract_t *g_tab;
static size_t g_n;

void tool_contract_bind(const tool_contract_t *tab, size_t n)
{
    g_tab = tab;
    g_n = tab ? n : 0;
}

size_t tool_contract_n(void)
{
    return g_n;
}

const tool_contract_t *tool_contract_at(size_t idx)
{
    return idx < g_n ? &g_tab[idx] : NULL;
}

const tool_contract_t *tool_contract_of(const char *tool_id)
{
    if (!tool_id)
        return NULL;
    for (size_t i = 0; i < g_n; ++i) {
        if (g_tab[i].id && strcmp(g_tab[i].id, tool_id) == 0)
            return &g_tab[i];
    }
    return NULL;
}

int tool_contract_env(tool_contract_env_t *env)
{
    if (!env)
        return -1;
    env->path_base[0] = '\0';
    return os_sandbox_fs_workspace(env->path_base, sizeof(env->path_base));
}

int tool_contract_scope_put(struct cJSON *obj, const char *base, int fenced)
{
    if (!obj || !base || base[0] == '\0')
        return -1;
    cJSON *scope = cJSON_CreateArray();
    if (!scope)
        return -1;
    if (fenced)
        cJSON_AddItemToArray(scope, cJSON_CreateString(base));
    cJSON_AddItemToObject(obj, "sandbox_scope", scope);
    return 0;
}

static cJSON *fx_array(unsigned fx)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    if (fx & TOOL_FX_FS)
        cJSON_AddItemToArray(arr, cJSON_CreateString("fs"));
    if (fx & TOOL_FX_NET)
        cJSON_AddItemToArray(arr, cJSON_CreateString("net"));
    if (fx & TOOL_FX_PROC)
        cJSON_AddItemToArray(arr, cJSON_CreateString("process"));
    if (cJSON_GetArraySize(arr) == 0)
        cJSON_AddItemToArray(arr, cJSON_CreateString("none"));
    return arr;
}

int tool_contract_put(cJSON *obj, const char *tool_id, const tool_contract_env_t *env)
{
    if (!obj)
        return -1;
    const tool_contract_t *c = tool_contract_of(tool_id);
    if (!c)
        return 0; /* 插件工具未声明契约：不伪造字段 */
    if (!env || env->path_base[0] == '\0')
        return -1;

    cJSON *x = cJSON_CreateObject();
    if (!x)
        return -1;
    cJSON_AddStringToObject(x, "path_base", env->path_base);
    tool_contract_scope_put(x, env->path_base, c->fenced);

    if (c->dep_name)
        cJSON_AddStringToObject(x, "requires_daemon", c->dep_name);
    else
        cJSON_AddNullToObject(x, "requires_daemon");
    cJSON_AddBoolToObject(x, "creates_parents", c->mkdirs ? 1 : 0);

    cJSON *fx = fx_array(c->fx);
    if (fx)
        cJSON_AddItemToObject(x, "side_effects", fx);

    cJSON_AddItemToObject(obj, "x-airy", x);
    return 0;
}
