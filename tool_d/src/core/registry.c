// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file registry.c
 * @brief Tool-registry implementation (in-memory hash table).
 */

#include "error.h"
#include "daemon_platform_ext.h"
#include "core/registry.h"
#include "core/tool_contract.h"
#include "tool_service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

#define HASH_SIZE 256

typedef struct registry_entry {
    char *id;
    tool_metadata_t *meta;
    struct registry_entry *next;
} registry_entry_t;

struct tool_registry {
    registry_entry_t *buckets[HASH_SIZE];
    airy_mtx_t lock;
    /* D8：可用性谓词（NULL = 不过滤）。由策略层（service.c）注入。 */
    tool_up_fn up;
    void *up_ud;
};

static unsigned int hash(const char *id)
{
    unsigned int h = 5381;
    while (*id)
        h = (h << 5) + h + *id++;
    return h % HASH_SIZE;
}

static void param_arr_free(tool_param_t *p, size_t n)
{
    if (!p)
        return;
    for (size_t i = 0; i < n; ++i) {
        AIRY_FREE((void *)p[i].name);
        AIRY_FREE((void *)p[i].schema);
    }
    AIRY_FREE((void *)p);
}

/* 参数名 → JSON Schema 类型推断。按 integer → boolean → number 的
 * 优先级顺序匹配关键字，与历史 if-else 链的短路顺序严格一致，保证
 * 同一个名字命中多个关键字时结果不漂移。 */
static const char *meta_type_hint(const char *pname)
{
    static const struct {
        const char *word;
        const char *type;
    } k_hints[] = {
        {"count", "integer"}, {"num", "integer"},     {"size", "integer"},
        {"port", "integer"},  {"timeout", "integer"}, {"limit", "integer"},
        {"enable", "boolean"}, {"flag", "boolean"},   {"verbose", "boolean"},
        {"force", "boolean"},  {"rate", "number"},    {"ratio", "number"},
        {"threshold", "number"}, {"score", "number"},
    };
    for (size_t i = 0; i < sizeof(k_hints) / sizeof(k_hints[0]); ++i) {
        if (strstr(pname, k_hints[i].word))
            return k_hints[i].type;
    }
    return "string";
}

static size_t strv_len(char *const *v)
{
    size_t n = 0;
    while (v[n])
        n++;
    return n;
}

/* 字符串参数名表 → tool_param_t 数组（name + 名字推断出的 schema）。
 * 空名保留 "{}"；任一失败整体回收并返回 NULL。 */
static tool_param_t *meta_params_fill(char *const *names, size_t cnt)
{
    tool_param_t *params = AIRY_CALLOC(cnt, sizeof(tool_param_t));
    if (!params)
        return NULL;
    for (size_t i = 0; i < cnt; ++i) {
        const char *pname = names[i];
        params[i].name = AIRY_STRDUP(pname);
        if (!params[i].name) {
            param_arr_free(params, cnt);
            return NULL;
        }
        char schema[128];
        if (pname[0])
            snprintf(schema, sizeof(schema), "{\"type\":\"%s\"}",
                     meta_type_hint(pname));
        else
            snprintf(schema, sizeof(schema), "{}");
        params[i].schema = AIRY_STRDUP(schema);
        if (!params[i].schema) {
            param_arr_free(params, cnt);
            return NULL;
        }
    }
    return params;
}

/* tool_def → 注册元数据。参数表构建失败不阻断注册（与原语义一致：
 * 注册仍以无参形态落库，调用方按 param_count 决定是否回收）。 */
static tool_metadata_t meta_from_def(const tool_def_t *def, tool_param_t **params)
{
    tool_metadata_t meta = {
        .id = def->name,
        .name = def->name,
        .executable = def->executable,
        .timeout_sec = def->timeout_sec,
        .cacheable = def->cacheable,
        .permission_rule = def->permission_rule,
    };
    *params = NULL;
    if (!def->params)
        return meta;
    size_t cnt = strv_len(def->params);
    if (cnt > 0) {
        *params = meta_params_fill(def->params, cnt);
        meta.params = *params;
        meta.param_count = *params ? cnt : 0;
    }
    return meta;
}

tool_registry_t *tool_registry_create(const tool_config_t *cfg)
{
    tool_registry_t *reg = AIRY_CALLOC(1, sizeof(tool_registry_t));
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    airy_mtx_init(&reg->lock);
    if (!cfg || !cfg->tools)
        return reg;
    for (tool_def_t *def = cfg->tools; def->name; ++def) {
        tool_param_t *params = NULL;
        tool_metadata_t meta = meta_from_def(def, &params);
        tool_registry_add(reg, &meta);
        param_arr_free(params, meta.param_count);
    }
    return reg;
}

void tool_registry_destroy(tool_registry_t *reg)
{
    if (!reg)
        return;
    airy_mtx_lock(&reg->lock);
    for (int i = 0; i < HASH_SIZE; ++i) {
        registry_entry_t *e = reg->buckets[i];
        while (e) {
            registry_entry_t *next = e->next;
            AIRY_FREE(e->id);
            tool_metadata_free(e->meta);
            AIRY_FREE(e);
            e = next;
        }
    }
    airy_mtx_unlock(&reg->lock);
    airy_mtx_destroy(&reg->lock);
    AIRY_FREE(reg);
}

int tool_registry_add(tool_registry_t *reg, const tool_metadata_t *meta)
{
    if (!reg || !meta || !meta->id)
        return AIRY_ERR_INVALID_PARAM;
    unsigned int idx = hash(meta->id);
    airy_mtx_lock(&reg->lock);
    for (registry_entry_t *e = reg->buckets[idx]; e; e = e->next) {
        if (strcmp(e->id, meta->id) == 0) {
            airy_mtx_unlock(&reg->lock);
            return AIRY_ERR_ALREADY_EXISTS;
        }
    }
    registry_entry_t *e = AIRY_CALLOC(1, sizeof(registry_entry_t));
    if (!e) {
        airy_mtx_unlock(&reg->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    e->id = AIRY_STRDUP(meta->id);
    e->meta = tool_metadata_clone(meta);
    if (!e->id || !e->meta) {
        AIRY_FREE(e->id);
        AIRY_FREE(e->meta);
        AIRY_FREE(e);
        airy_mtx_unlock(&reg->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    e->next = reg->buckets[idx];
    reg->buckets[idx] = e;
    airy_mtx_unlock(&reg->lock);
    return 0;
}

tool_metadata_t *tool_registry_get(tool_registry_t *reg, const char *tool_id)
{
    if (!reg || !tool_id) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    unsigned int idx = hash(tool_id);
    airy_mtx_lock(&reg->lock);
    for (registry_entry_t *e = reg->buckets[idx]; e; e = e->next) {
        if (strcmp(e->id, tool_id) == 0) {
            tool_metadata_t *copy = tool_metadata_clone(e->meta);
            airy_mtx_unlock(&reg->lock);
            return copy;
        }
    }
    airy_mtx_unlock(&reg->lock);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

void tool_registry_filter(tool_registry_t *reg, tool_up_fn fn, void *ud)
{
    if (!reg)
        return;
    airy_mtx_lock(&reg->lock);
    reg->up = fn;
    reg->up_ud = ud;
    airy_mtx_unlock(&reg->lock);
}

char *tool_registry_list_json(tool_registry_t *reg)
{
    if (!reg)
        return AIRY_STRDUP("[]");
    /* 契约环境含 getcwd/realpath，属 I/O：必须在注册表锁外解析。 */
    tool_contract_env_t cenv;
    int has_contract = (tool_contract_env(&cenv) == 0);
    airy_mtx_lock(&reg->lock);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < HASH_SIZE; ++i) {
        for (registry_entry_t *e = reg->buckets[i]; e; e = e->next) {
            /* D8：只输出此刻可暴露的工具，agent 与呈现层共用同一份清单，
             * 从根上消除"暴露成功 → 调用三连败"的时序矛盾。 */
            if (reg->up && !reg->up(e->meta->id, reg->up_ud))
                continue;
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "id", e->meta->id);
            cJSON_AddStringToObject(obj, "name", e->meta->name);
            cJSON_AddStringToObject(obj, "description",
                                    e->meta->description ? e->meta->description : "");
            cJSON_AddNumberToObject(obj, "cacheable", e->meta->cacheable);
            /* M1-1a 工具目录 SSoT：list_tools 输出每个工具的 inputSchema
             * （OpenAI tools parameters 格式，从 tool_param_t 构造），
             * 使 tool_d 成为工具目录唯一权威，gateway MCP 注册从本 RPC 拉取。 */
            cJSON *schema = cJSON_CreateObject();
            if (schema) {
                cJSON_AddStringToObject(schema, "type", "object");
                cJSON *props = cJSON_CreateObject();
                cJSON *required = cJSON_CreateArray();
                for (size_t p = 0; p < e->meta->param_count; p++) {
                    cJSON *ps = cJSON_Parse(e->meta->params[p].schema);
                    if (ps)
                        cJSON_AddItemToObject(props, e->meta->params[p].name, ps);
                    if (e->meta->params[p].required)
                        cJSON_AddItemToArray(required,
                                             cJSON_CreateString(e->meta->params[p].name));
                }
                cJSON_AddItemToObject(schema, "properties", props);
                if (cJSON_GetArraySize(required) > 0)
                    cJSON_AddItemToObject(schema, "required", required);
                else
                    cJSON_Delete(required);
                cJSON_AddItemToObject(obj, "input_schema", schema);
            }
            if (has_contract)
                tool_contract_put(obj, e->meta->id, &cenv);
            cJSON_AddItemToArray(arr, obj);
        }
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    airy_mtx_unlock(&reg->lock);
    return json;
}
