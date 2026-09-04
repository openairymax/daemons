// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_registry.c
 * @brief Tool 注册表单元测试
 */

#include "registry.h"

#include "airy_memory.h"
#include "airy_tool_schema.h"
#include "tool_service.h"

#include <cjson/cJSON.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 测试注册表创建和销毁
 */
static void test_registry_create_destroy(void)
{
    printf("  test_registry_create_destroy...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief 测试工具注册
 */
static void test_registry_add(void)
{
    printf("  test_registry_add...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "test_tool_001";
    meta.name = "Test Tool";
    meta.description = "A test tool for unit testing";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 30;
    meta.cacheable = 1;

    int ret __attribute__((unused)) = tool_registry_add(reg, &meta);
    assert(ret == 0);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test duplicate tool registration
 */
static void test_registry_add_duplicate(void)
{
    printf("  test_registry_add_duplicate...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "duplicate_tool";
    meta.name = "Duplicate Tool";
    meta.executable = "/bin/echo";

    int ret __attribute__((unused)) = tool_registry_add(reg, &meta);
    assert(ret == 0);

    ret = tool_registry_add(reg, &meta);
    assert(ret != 0);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test tool retrieval
 */
static void test_registry_get(void)
{
    printf("  test_registry_get...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "get_test_tool";
    meta.name = "Get Test Tool";
    meta.description = "Tool for get testing";
    meta.executable = "/bin/cat";

    tool_registry_add(reg, &meta);

    tool_metadata_t *retrieved = tool_registry_get(reg, "get_test_tool");
    assert(retrieved != NULL);
    assert(strcmp(retrieved->id, "get_test_tool") == 0);
    assert(strcmp(retrieved->name, "Get Test Tool") == 0);

    tool_metadata_free(retrieved);
    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test tool retrieval of a nonexistent tool
 */
static void test_registry_get_nonexistent(void)
{
    printf("  test_registry_get_nonexistent...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t *retrieved __attribute__((unused)) = tool_registry_get(reg, "nonexistent_tool");
    assert(retrieved == NULL);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test tool removal
 */
static void test_registry_remove(void)
{
    printf("  test_registry_remove...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "remove_test_tool";
    meta.name = "Remove Test Tool";
    meta.executable = "/bin/ls";

    tool_registry_add(reg, &meta);

    int ret __attribute__((unused)) = tool_registry_remove(reg, "remove_test_tool");
    assert(ret == 0);

    tool_metadata_t *retrieved __attribute__((unused)) = tool_registry_get(reg, "remove_test_tool");
    assert(retrieved == NULL);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test removal of a nonexistent tool
 */
static void test_registry_remove_nonexistent(void)
{
    printf("  test_registry_remove_nonexistent...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    int ret __attribute__((unused)) = tool_registry_remove(reg, "nonexistent_tool");
    assert(ret != 0);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test tool list as JSON
 */
static void test_registry_list_json(void)
{
    printf("  test_registry_list_json...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_metadata_t meta1;
    AIRY_MEMSET(&meta1, 0, sizeof(meta1));
    meta1.id = "json_tool_1";
    meta1.name = "JSON Tool 1";
    meta1.executable = "/bin/echo";

    tool_metadata_t meta2;
    AIRY_MEMSET(&meta2, 0, sizeof(meta2));
    meta2.id = "json_tool_2";
    meta2.name = "JSON Tool 2";
    meta2.executable = "/bin/cat";

    tool_registry_add(reg, &meta1);
    tool_registry_add(reg, &meta2);

    char *json = tool_registry_list_json(reg);
    assert(json != NULL);
    assert(strstr(json, "json_tool_1") != NULL);
    assert(strstr(json, "json_tool_2") != NULL);

    free(json);
    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test empty-registry list JSON
 */
static void test_registry_list_json_empty(void)
{
    printf("  test_registry_list_json_empty...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    char *json = tool_registry_list_json(reg);
    assert(json != NULL);
    assert(strcmp(json, "[]") == 0);

    free(json);
    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test registry null parameters
 */
static void test_registry_null_param(void)
{
    printf("  test_registry_null_param...\n");

    int ret __attribute__((unused)) = tool_registry_add(NULL, NULL);
    assert(ret != 0);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = NULL;

    tool_registry_t *reg = tool_registry_create(NULL);
    ret = tool_registry_add(reg, &meta);
    assert(ret != 0);

    tool_metadata_t *retrieved __attribute__((unused)) = tool_registry_get(NULL, "test");
    assert(retrieved == NULL);

    retrieved = tool_registry_get(reg, NULL);
    assert(retrieved == NULL);

    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief Test a tool with parameters
 */
static void test_registry_tool_with_params(void)
{
    printf("  test_registry_tool_with_params...\n");

    tool_registry_t *reg = tool_registry_create(NULL);
    assert(reg != NULL);

    tool_param_t params[2];
    AIRY_MEMSET(params, 0, sizeof(params));
    params[0].name = "input_file";
    params[0].schema = "{\"type\": \"string\"}";
    params[1].name = "output_file";
    params[1].schema = "{\"type\": \"string\"}";

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "param_tool";
    meta.name = "Param Tool";
    meta.executable = "/usr/bin/cp";
    meta.params = params;
    meta.param_count = 2;

    int ret __attribute__((unused)) = tool_registry_add(reg, &meta);
    assert(ret == 0);

    tool_metadata_t *retrieved __attribute__((unused)) = tool_registry_get(reg, "param_tool");
    assert(retrieved != NULL);
    assert(retrieved->param_count == 2);
    assert(strcmp(retrieved->params[0].name, "input_file") == 0);
    assert(strcmp(retrieved->params[1].name, "output_file") == 0);

    tool_metadata_free(retrieved);
    tool_registry_destroy(reg);

    printf("    PASSED\n");
}

/**
 * @brief 测试工具目录与 commons 契约层一致性（M1-1a 工具目录 SSoT 门禁）
 *
 * tool_d 的 builtin 工具目录（list_tools 输出的 input_schema）是工具
 * 目录唯一权威；gateway MCP 注册从 tool_d 拉取。本测试验证该目录与
 * commons 契约层（airy_tool_schema.h，LLM 工具 schema 单一权威）：
 *   1. 工具名集合双向一致；
 *   2. 每个契约层声明参数的 required 状态与 tool_d 一致（S-6 漂移根因）；
 *   3. 契约层每个参数在 tool_d 中存在（tool_d 允许声明额外可选参数，
 *      如 shell_run.cwd，供 CLI 直接调用，LLM 视图不暴露）。
 */
static void test_builtin_catalog_matches_contract(void)
{
    printf("  test_builtin_catalog_matches_contract...\n");

    /* 契约层解析：AIRY_TOOLS_JSON_SOURCE -> {name: cJSON function} */
    cJSON *contract = cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    assert(contract != NULL);
    int contract_n = cJSON_GetArraySize(contract);
    assert(contract_n == 15);

    /* tool_d 内置工具目录：service_create 内已 register_builtin_tools */
    tool_service_t *svc = tool_service_create(NULL);
    assert(svc != NULL);
    char *list = tool_service_list(svc);
    assert(list != NULL);
    cJSON *catalog = cJSON_Parse(list);
    AIRY_FREE(list);
    assert(catalog != NULL);
    int catalog_n = cJSON_GetArraySize(catalog);
    assert(catalog_n == contract_n);

    for (int i = 0; i < contract_n; i++) {
        cJSON *cfn = cJSON_GetArrayItem(contract, i);
        cJSON *f = cJSON_GetObjectItem(cfn, "function");
        assert(f != NULL);
        const char *cname = cJSON_GetObjectItem(f, "name")->valuestring;
        cJSON *cparams = cJSON_GetObjectItem(f, "parameters");
        assert(cparams != NULL);

        /* 契约层工具在 tool_d 目录中必须存在 */
        cJSON *entry = NULL;
        int found = 0;
        for (int j = 0; j < catalog_n; j++) {
            cJSON *e = cJSON_GetArrayItem(catalog, j);
            cJSON *ename = cJSON_GetObjectItem(e, "name");
            if (ename && strcmp(ename->valuestring, cname) == 0) {
                entry = e;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "contract tool %s missing in tool_d catalog\n", cname);
        }
        assert(found);

        cJSON *dschema = cJSON_GetObjectItem(entry, "input_schema");
        assert(dschema != NULL);

        /* required 语义一致 */
        cJSON *creq = cJSON_GetObjectItem(cparams, "required");
        cJSON *dreq = cJSON_GetObjectItem(dschema, "required");
        int cn = creq ? cJSON_GetArraySize(creq) : 0;
        int dn = dreq ? cJSON_GetArraySize(dreq) : 0;
        assert(cn == dn);
        for (int r = 0; r < cn; r++) {
            const char *rn = cJSON_GetArrayItem(creq, r)->valuestring;
            int in_d = 0;
            for (int r2 = 0; r2 < dn; r2++) {
                if (strcmp(cJSON_GetArrayItem(dreq, r2)->valuestring, rn) == 0)
                    in_d = 1;
            }
            if (!in_d) {
                fprintf(stderr, "tool %s: required param %s missing in tool_d\n", cname, rn);
            }
            assert(in_d);
        }

        /* 契约层每个参数在 tool_d 中存在 */
        cJSON *cprops = cJSON_GetObjectItem(cparams, "properties");
        cJSON *dprops = cJSON_GetObjectItem(dschema, "properties");
        assert(cprops != NULL && dprops != NULL);
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, cprops)
        {
            const char *pname = p->string;
            cJSON *dp = cJSON_GetObjectItem(dprops, pname);
            if (!dp) {
                fprintf(stderr, "tool %s: param %s missing in tool_d\n", cname, pname);
            }
            assert(dp != NULL);
        }
    }

    cJSON_Delete(catalog);
    tool_service_destroy(svc);
    cJSON_Delete(contract);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Registry Unit Tests\n");
    printf("=========================================\n");

    test_registry_create_destroy();
    test_registry_add();
    test_registry_add_duplicate();
    test_registry_get();
    test_registry_get_nonexistent();
    test_registry_remove();
    test_registry_remove_nonexistent();
    test_registry_list_json();
    test_registry_list_json_empty();
    test_registry_null_param();
    test_registry_tool_with_params();
    test_builtin_catalog_matches_contract();

    printf("\n✅ All tool registry tests PASSED\n");
    return 0;
}
