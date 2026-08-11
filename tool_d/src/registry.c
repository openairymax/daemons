// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file registry.c
 * @brief 工具注册表实现（内存哈希表）
 */

#include "error.h"
#include "daemon_platform_ext.h"
#include "registry.h"
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
};

static unsigned int hash(const char *id)
{
    unsigned int h = 5381;
    while (*id)
        h = (h << 5) + h + *id++;
    return h % HASH_SIZE;
}

static tool_metadata_t *dup_metadata(const tool_metadata_t *src)
{
    tool_metadata_t *dst = AIRY_CALLOC(1, sizeof(tool_metadata_t));
    if (!dst) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    dst->id = AIRY_STRDUP(src->id);
    if (!dst->id) {
        AIRY_FREE(dst);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate id");
    }
    dst->name = AIRY_STRDUP(src->name);
    if (!dst->name) {
        AIRY_FREE(dst->id);
        AIRY_FREE(dst);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate name");
    }
    dst->description = src->description ? AIRY_STRDUP(src->description) : NULL;
    if (src->description && !dst->description) {
        AIRY_FREE(dst->name);
        AIRY_FREE(dst->id);
        AIRY_FREE(dst);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate description");
    }
    dst->executable = AIRY_STRDUP(src->executable);
    if (!dst->executable) {
        AIRY_FREE(dst->description);
        AIRY_FREE(dst->name);
        AIRY_FREE(dst->id);
        AIRY_FREE(dst);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate executable");
    }
    dst->timeout_sec = src->timeout_sec;
    dst->cacheable = src->cacheable;
    dst->permission_rule = src->permission_rule ? AIRY_STRDUP(src->permission_rule) : NULL;
    if (src->permission_rule && !dst->permission_rule) {
        AIRY_FREE(dst->executable);
        AIRY_FREE(dst->description);
        AIRY_FREE(dst->name);
        AIRY_FREE(dst->id);
        AIRY_FREE(dst);
        AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate permission_rule");
    }
    if (src->param_count > 0) {
        dst->params = AIRY_CALLOC(src->param_count, sizeof(tool_param_t));
        if (!dst->params) {
            AIRY_FREE(dst->permission_rule);
            AIRY_FREE(dst->executable);
            AIRY_FREE(dst->description);
            AIRY_FREE(dst->name);
            AIRY_FREE(dst->id);
            AIRY_FREE(dst);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }
        for (size_t i = 0; i < src->param_count; ++i) {
            dst->params[i].name = AIRY_STRDUP(src->params[i].name);
            if (!dst->params[i].name) {
                for (size_t k = 0; k < i; ++k) {
                    AIRY_FREE(dst->params[k].name);
                    AIRY_FREE(dst->params[k].schema);
                }
                AIRY_FREE(dst->params);
                AIRY_FREE(dst->permission_rule);
                AIRY_FREE(dst->executable);
                AIRY_FREE(dst->description);
                AIRY_FREE(dst->name);
                AIRY_FREE(dst->id);
                AIRY_FREE(dst);
                AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate param name");
            }
            dst->params[i].schema = AIRY_STRDUP(src->params[i].schema);
            if (!dst->params[i].schema) {
                AIRY_FREE(dst->params[i].name);
                for (size_t k = 0; k < i; ++k) {
                    AIRY_FREE(dst->params[k].name);
                    AIRY_FREE(dst->params[k].schema);
                }
                AIRY_FREE(dst->params);
                AIRY_FREE(dst->permission_rule);
                AIRY_FREE(dst->executable);
                AIRY_FREE(dst->description);
                AIRY_FREE(dst->name);
                AIRY_FREE(dst->id);
                AIRY_FREE(dst);
                AIRY_ERROR_NULL(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate param schema");
            }
        }
        dst->param_count = src->param_count;
    }
    return dst;
}

tool_registry_t *tool_registry_create(const tool_config_t *cfg)
{
    tool_registry_t *reg = AIRY_CALLOC(1, sizeof(tool_registry_t));
    if (!reg) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    airy_mtx_init(&reg->lock);

    if (cfg && cfg->tools) {
        for (tool_def_t *def = cfg->tools; def->name; ++def) {
            tool_metadata_t meta = {
                .id = def->name,
                .name = def->name,
                .executable = def->executable,
                .timeout_sec = def->timeout_sec,
                .cacheable = def->cacheable,
                .permission_rule = def->permission_rule,
            };
            if (def->params) {
                size_t cnt = 0;
                while (def->params[cnt])
                    cnt++;
                if (cnt > 0) {
                    tool_param_t *params = AIRY_CALLOC(cnt, sizeof(tool_param_t));
                    if (params) {
                        for (size_t i = 0; i < cnt; ++i) {
                            params[i].name = AIRY_STRDUP(def->params[i]);
                            if (!params[i].name) {
                                for (size_t k = 0; k < i; ++k) {
                                    AIRY_FREE((void *)params[k].name);
                                    AIRY_FREE((void *)params[k].schema);
                                }
                                AIRY_FREE(params);
                                params = NULL;
                                break;
                            }
                            const char *pname = def->params[i];
                            size_t pname_len = pname ? strlen(pname) : 0;
                            char schema[128];
                            if (pname_len > 0) {
                                const char *type_hint = "string";
                                if (strstr(pname, "count") || strstr(pname, "num") ||
                                    strstr(pname, "size") || strstr(pname, "port") ||
                                    strstr(pname, "timeout") || strstr(pname, "limit")) {
                                    type_hint = "integer";
                                } else if (strstr(pname, "enable") || strstr(pname, "flag") ||
                                           strstr(pname, "verbose") || strstr(pname, "force")) {
                                    type_hint = "boolean";
                                } else if (strstr(pname, "rate") || strstr(pname, "ratio") ||
                                           strstr(pname, "threshold") || strstr(pname, "score")) {
                                    type_hint = "number";
                                }
                                snprintf(schema, sizeof(schema), "{\"type\":\"%s\"}", type_hint);
                            } else {
                                snprintf(schema, sizeof(schema), "{}");
                            }
                            params[i].schema = AIRY_STRDUP(schema);
                            if (!params[i].schema) {
                                AIRY_FREE(params[i].name);
                                for (size_t k = 0; k < i; ++k) {
                                    AIRY_FREE((void *)params[k].name);
                                    AIRY_FREE((void *)params[k].schema);
                                }
                                AIRY_FREE(params);
                                params = NULL;
                                break;
                            }
                        }
                        if (params) {
                            meta.params = params;
                            meta.param_count = cnt;
                        }
                    }
                }
            }
            tool_registry_add(reg, &meta);
            if (meta.params) {
                for (size_t i = 0; i < meta.param_count; ++i) {
                    AIRY_FREE((void *)meta.params[i].name);
                    AIRY_FREE((void *)meta.params[i].schema);
                }
                AIRY_FREE((void *)meta.params);
            }
        }
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
    e->meta = dup_metadata(meta);
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

int tool_registry_remove(tool_registry_t *reg, const char *tool_id)
{
    if (!reg || !tool_id)
        return AIRY_ERR_INVALID_PARAM;
    unsigned int idx = hash(tool_id);
    airy_mtx_lock(&reg->lock);
    registry_entry_t **p = &reg->buckets[idx];
    while (*p) {
        if (strcmp((*p)->id, tool_id) == 0) {
            registry_entry_t *victim = *p;
            *p = victim->next;
            AIRY_FREE(victim->id);
            tool_metadata_free(victim->meta);
            AIRY_FREE(victim);
            airy_mtx_unlock(&reg->lock);
            return 0;
        }
        p = &(*p)->next;
    }
    airy_mtx_unlock(&reg->lock);
    return AIRY_ERR_NOT_FOUND;
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
            tool_metadata_t *copy = dup_metadata(e->meta);
            airy_mtx_unlock(&reg->lock);
            return copy;
        }
    }
    airy_mtx_unlock(&reg->lock);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

char *tool_registry_list_json(tool_registry_t *reg)
{
    if (!reg)
        return AIRY_STRDUP("[]");
    airy_mtx_lock(&reg->lock);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < HASH_SIZE; ++i) {
        for (registry_entry_t *e = reg->buckets[i]; e; e = e->next) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "id", e->meta->id);
            cJSON_AddStringToObject(obj, "name", e->meta->name);
            cJSON_AddStringToObject(obj, "description",
                                    e->meta->description ? e->meta->description : "");
            cJSON_AddNumberToObject(obj, "cacheable", e->meta->cacheable);
            cJSON_AddItemToArray(arr, obj);
        }
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    airy_mtx_unlock(&reg->lock);
    return json;
}
