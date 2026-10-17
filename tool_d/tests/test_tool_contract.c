// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_tool_contract.c
 * @brief 工具契约边车单元测试（N2）：五字段投影、未声明不伪造、
 *        依赖同源、可用性过滤后的清单一致性。
 */

#include "core/registry.h"
#include "core/tool_contract.h"
#include "airy_memory.h"
#include "rpc/service.h"
#include "sandbox/os_sandbox.h"
#include "tool_service.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CHECK
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#endif

static cJSON *find_tool(cJSON *arr, const char *id)
{
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0)
            return it;
    }
    return NULL;
}

static int drop_maths(const char *tool_id, void *ud)
{
    (void)ud;
    return strncmp(tool_id, "maths_", 6) != 0;
}

static void test_contract_env(void)
{
    printf("  test_contract_env...\n");

    tool_contract_env_t env;
    CHECK(tool_contract_env(&env) == 0);
    CHECK(env.path_base[0] != '\0');

    char ws[TOOL_CONTRACT_PATH_MAX];
    CHECK(os_sandbox_fs_workspace(ws, sizeof(ws)) == 0);
    CHECK(strcmp(env.path_base, ws) == 0);

    CHECK(tool_contract_env(NULL) != 0);

    printf("    PASSED\n");
}

static void test_contract_scope(void)
{
    printf("  test_contract_scope...\n");

    tool_contract_env_t env;
    CHECK(tool_contract_env(&env) == 0);

    cJSON *obj = cJSON_CreateObject();
    CHECK(obj != NULL);
    CHECK(tool_scope_put(obj, env.path_base, 1) == 0);
    cJSON *scope = cJSON_GetObjectItem(obj, "sandbox_scope");
    CHECK(cJSON_IsArray(scope) && cJSON_GetArraySize(scope) == 1);
    CHECK(strcmp(cJSON_GetArrayItem(scope, 0)->valuestring, env.path_base) == 0);
    cJSON_Delete(obj);

    obj = cJSON_CreateObject();
    CHECK(obj != NULL);
    CHECK(tool_scope_put(obj, env.path_base, 0) == 0);
    scope = cJSON_GetObjectItem(obj, "sandbox_scope");
    CHECK(cJSON_IsArray(scope) && cJSON_GetArraySize(scope) == 0);
    cJSON_Delete(obj);

    obj = cJSON_CreateObject();
    CHECK(obj != NULL);
    CHECK(tool_scope_put(obj, "", 1) != 0);
    CHECK(tool_scope_put(obj, NULL, 1) != 0);
    CHECK(cJSON_GetObjectItem(obj, "sandbox_scope") == NULL);
    cJSON_Delete(obj);

    printf("    PASSED\n");
}

static void test_contract_unknown(void)
{
    printf("  test_contract_unknown...\n");

    tool_contract_env_t env;
    CHECK(tool_contract_env(&env) == 0);

    cJSON *obj = cJSON_CreateObject();
    CHECK(obj != NULL);
    CHECK(tool_contract_put(obj, "plugin_undeclared", &env) == 0);
    CHECK(cJSON_GetObjectItem(obj, "x-airy") == NULL);
    cJSON_Delete(obj);

    printf("    PASSED\n");
}

static void test_builtin_contract(void)
{
    printf("  test_builtin_contract...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);
    CHECK(tool_contract_n() >= 16);

    const tool_contract_t *w = tool_contract_of("fs_write");
    CHECK(w != NULL && w->mkdirs && w->fenced && (w->fx & TOOL_FX_FS));
    const tool_contract_t *m = tool_contract_of("maths_eval");
    CHECK(m != NULL && m->dep_sock != NULL && m->dep_name != NULL);
    CHECK(strcmp(m->dep_sock, "maths.sock") == 0);
    CHECK(strcmp(m->dep_name, "maths_d") == 0);
    CHECK(tool_contract_of("no_such_tool") == NULL);

    char *json = tool_registry_list_json(svc->registry);
    CHECK(json != NULL);
    cJSON *arr = cJSON_Parse(json);
    free(json);
    CHECK(arr != NULL);
    CHECK(cJSON_GetArraySize(arr) == 16);

    tool_contract_env_t env;
    CHECK(tool_contract_env(&env) == 0);

    cJSON *wobj = find_tool(arr, "fs_write");
    CHECK(wobj != NULL);
    cJSON *x = cJSON_GetObjectItem(wobj, "x-airy");
    CHECK(x != NULL);
    cJSON *pb = cJSON_GetObjectItem(x, "path_base");
    CHECK(cJSON_IsString(pb) && strcmp(pb->valuestring, env.path_base) == 0);
    cJSON *scope = cJSON_GetObjectItem(x, "sandbox_scope");
    CHECK(cJSON_IsArray(scope) && cJSON_GetArraySize(scope) == 1);
    CHECK(strcmp(cJSON_GetArrayItem(scope, 0)->valuestring, env.path_base) == 0);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(x, "requires_daemon")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(x, "creates_parents")));
    cJSON *fx = cJSON_GetObjectItem(x, "side_effects");
    CHECK(cJSON_IsArray(fx) && cJSON_GetArraySize(fx) == 1);
    CHECK(strcmp(cJSON_GetArrayItem(fx, 0)->valuestring, "fs") == 0);

    cJSON *robj = find_tool(arr, "fs_read");
    CHECK(robj != NULL);
    cJSON *rx = cJSON_GetObjectItem(robj, "x-airy");
    CHECK(rx != NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(rx, "sandbox_scope")) == 0);
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(rx, "creates_parents")));

    cJSON *sobj = find_tool(arr, "shell_run");
    CHECK(sobj != NULL);
    cJSON *sfx = cJSON_GetObjectItem(cJSON_GetObjectItem(sobj, "x-airy"), "side_effects");
    CHECK(cJSON_GetArraySize(sfx) == 3);

    cJSON *mobj = find_tool(arr, "maths_eval");
    CHECK(mobj != NULL);
    cJSON *mreq = cJSON_GetObjectItem(cJSON_GetObjectItem(mobj, "x-airy"), "requires_daemon");
    CHECK(cJSON_IsString(mreq) && strcmp(mreq->valuestring, "maths_d") == 0);

    cJSON_Delete(arr);
    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_avail_filter(void)
{
    printf("  test_avail_filter...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "plugin_x";
    meta.name = "plugin_x";
    meta.executable = "libplugin_x.so";
    meta.timeout_sec = 10;
    CHECK(tool_registry_add(svc->registry, &meta) == 0);

    char *json = tool_registry_list_json(svc->registry);
    CHECK(json != NULL);
    cJSON *arr = cJSON_Parse(json);
    free(json);
    CHECK(arr != NULL);
    cJSON *pobj = find_tool(arr, "plugin_x");
    CHECK(pobj != NULL);
    CHECK(cJSON_GetObjectItem(pobj, "x-airy") == NULL);
    cJSON_Delete(arr);

    tool_registry_filter(svc->registry, drop_maths, NULL);
    json = tool_registry_list_json(svc->registry);
    CHECK(json != NULL);
    arr = cJSON_Parse(json);
    free(json);
    CHECK(arr != NULL);
    CHECK(find_tool(arr, "maths_eval") == NULL);
    CHECK(find_tool(arr, "fs_read") != NULL);
    cJSON_Delete(arr);

    tool_registry_filter(svc->registry, NULL, NULL);
    json = tool_registry_list_json(svc->registry);
    CHECK(json != NULL);
    arr = cJSON_Parse(json);
    free(json);
    CHECK(arr != NULL);
    CHECK(find_tool(arr, "maths_eval") != NULL);
    cJSON_Delete(arr);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

int main(void)
{
    printf("tool contract tests:\n");
    test_contract_env();
    test_contract_scope();
    test_contract_unknown();
    test_builtin_contract();
    test_avail_filter();
    printf("all tool contract tests passed.\n");
    return 0;
}
