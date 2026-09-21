// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file cost_tracker.c
 * @brief Cost-tracking implementation (matched by config rules).
 */

#include "cost_tracker.h"
#include "daemon_platform_ext.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

typedef struct model_cost {
    char *model;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    double cost_usd;
    struct model_cost *next;
} model_cost_t;

struct cost_tracker {
    pricing_rule_t *rules;
    int rule_count;
    model_cost_t *models;
    airy_mtx_t lock;
};

static int match_rule(const char *model, const pricing_rule_t *rule)
{
    if (!rule || !rule->model_pattern || !model)
        return 0;
    size_t len = strlen(rule->model_pattern);
    if (rule->model_pattern[len - 1] == '*') {
        return strncmp(model, rule->model_pattern, len - 1) == 0;
    }
    return strcmp(model, rule->model_pattern) == 0;
}

static void get_price(const cost_tracker_t *ct, const char *model, double *input_price,
                      double *output_price)
{
    *input_price = 0.001;
    *output_price = 0.002;
    for (int i = 0; i < ct->rule_count; ++i) {
        if (match_rule(model, &ct->rules[i])) {
            *input_price = ct->rules[i].input_price_per_k;
            *output_price = ct->rules[i].output_price_per_k;
            return;
        }
    }
}

cost_tracker_t *cost_tracker_create(const pricing_rule_t *rules, int rule_count)
{
    cost_tracker_t *ct = AIRY_CALLOC(1, sizeof(cost_tracker_t));
    if (!ct) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (rule_count > 0) {
        SAFE_MALLOC_ARRAY(ct->rules, rule_count, sizeof(pricing_rule_t));
        if (!ct->rules) {
            AIRY_FREE(ct);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }
        __builtin_memcpy(ct->rules, rules, rule_count * sizeof(pricing_rule_t));
        ct->rule_count = rule_count;
    }
    airy_mtx_init(&ct->lock);
    return ct;
}

void cost_tracker_destroy(cost_tracker_t *ct)
{
    if (!ct)
        return;
    airy_mtx_lock(&ct->lock);
    model_cost_t *m = ct->models;
    while (m) {
        model_cost_t *next = m->next;
        AIRY_FREE(m->model);
        AIRY_FREE(m);
        m = next;
    }
    airy_mtx_unlock(&ct->lock);
    airy_mtx_destroy(&ct->lock);
    AIRY_FREE(ct->rules);
    AIRY_FREE(ct);
}

void cost_tracker_add(cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                      uint32_t completion_tokens)
{
    if (!ct || !model)
        return;
    airy_mtx_lock(&ct->lock);
    model_cost_t *m = ct->models;
    while (m) {
        if (strcmp(m->model, model) == 0)
            break;
        m = m->next;
    }
    if (!m) {
        m = AIRY_CALLOC(1, sizeof(model_cost_t));
        if (!m) {
            airy_mtx_unlock(&ct->lock);
            return;
        }
        m->model = AIRY_STRDUP(model);
        m->next = ct->models;
        ct->models = m;
    }
    m->prompt_tokens += prompt_tokens;
    m->completion_tokens += completion_tokens;

    double in_price, out_price;
    get_price(ct, model, &in_price, &out_price);
    m->cost_usd += (prompt_tokens / 1000.0) * in_price + (completion_tokens / 1000.0) * out_price;
    airy_mtx_unlock(&ct->lock);
}

double cost_tracker_estimate(const cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                             uint32_t completion_tokens)
{
    if (!ct || !model)
        return 0.0;
    double in_price, out_price;
    get_price(ct, model, &in_price, &out_price);
    return (prompt_tokens / 1000.0) * in_price + (completion_tokens / 1000.0) * out_price;
}

cJSON *cost_tracker_export(cost_tracker_t *ct)
{
    if (!ct)
        return cJSON_CreateObject();
    airy_mtx_lock(&ct->lock);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    model_cost_t *m = ct->models;
    while (m) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "model", m->model);
        cJSON_AddNumberToObject(obj, "prompt_tokens", m->prompt_tokens);
        cJSON_AddNumberToObject(obj, "completion_tokens", m->completion_tokens);
        cJSON_AddNumberToObject(obj, "cost_usd", m->cost_usd);
        cJSON_AddItemToArray(arr, obj);
        m = m->next;
    }
    cJSON_AddItemToObject(root, "models", arr);
    airy_mtx_unlock(&ct->lock);
    return root;
}

/* 2.1.1.5 修复：计费/用量持久化。save 原子写（临时文件 + rename 防
 * 半写文件）；load 解析 JSON 并按模型合并累计（幂等）。 */
int cost_tracker_save(cost_tracker_t *ct, const char *path)
{
    if (!ct || !path || !path[0])
        return -1;

    cJSON *root = cost_tracker_export(ct);
    if (!root)
        return -1;
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return -1;

    char tmp[1024];
    int tlen = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tlen < 0 || tlen >= (int)sizeof(tmp)) {
        AIRY_FREE(json);
        return -1;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        AIRY_FREE(json);
        return -1;
    }
    size_t jlen = strlen(json);
    int wok = fwrite(json, 1, jlen, f) == jlen;
    if (wok && fflush(f) != 0)
        wok = 0;
    /* 落盘后 rename，防断电丢数据（rename 前不 fsync 只保证页缓存顺序） */
    if (wok) {
#ifdef _WIN32
        wok = (_commit(_fileno(f)) == 0);
#else
        wok = (fsync(fileno(f)) == 0);
#endif
    }
    fclose(f);
    AIRY_FREE(json);
    if (!wok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int cost_tracker_load(cost_tracker_t *ct, const char *path)
{
    if (!ct || !path || !path[0])
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return 0; /* 无历史文件不是错误 */

    long sz = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (sz == 0) {
        fclose(f);
        return 0;
    }
    if (sz > (long)(16 * 1024 * 1024)) { /* 16MiB 上限防畸形文件 */
        fclose(f);
        return -1;
    }
    char *buf = (char *)AIRY_MALLOC((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rn = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rn != (size_t)sz) {
        AIRY_FREE(buf);
        return -1;
    }
    buf[rn] = '\0';

    cJSON *root = cJSON_Parse(buf);
    AIRY_FREE(buf);
    if (!root)
        return -1;
    cJSON *arr = cJSON_GetObjectItem(root, "models");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return -1;
    }

    airy_mtx_lock(&ct->lock);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *mn = cJSON_GetObjectItem(item, "model");
        if (!cJSON_IsString(mn) || !mn->valuestring || !mn->valuestring[0])
            continue;
        cJSON *pt = cJSON_GetObjectItem(item, "prompt_tokens");
        cJSON *ctok = cJSON_GetObjectItem(item, "completion_tokens");
        cJSON *cost = cJSON_GetObjectItem(item, "cost_usd");
        uint32_t p = cJSON_IsNumber(pt) ? (uint32_t)pt->valuedouble : 0;
        uint32_t c = cJSON_IsNumber(ctok) ? (uint32_t)ctok->valuedouble : 0;
        double d = cJSON_IsNumber(cost) ? cost->valuedouble : 0.0;

        model_cost_t *m = ct->models;
        while (m) {
            if (strcmp(m->model, mn->valuestring) == 0)
                break;
            m = m->next;
        }
        if (!m) {
            m = (model_cost_t *)AIRY_CALLOC(1, sizeof(model_cost_t));
            if (!m)
                continue;
            m->model = AIRY_STRDUP(mn->valuestring);
            m->next = ct->models;
            ct->models = m;
        }
        m->prompt_tokens += p;
        m->completion_tokens += c;
        m->cost_usd += d;
    }
    airy_mtx_unlock(&ct->lock);
    cJSON_Delete(root);
    return 0;
}
