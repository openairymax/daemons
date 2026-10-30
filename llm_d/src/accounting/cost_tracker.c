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
    uint64_t cached_tokens;
    double cost_usd;
    struct model_cost *next;
} model_cost_t;

/* 落盘窗口：任一条满足即写。两个阈值共同界定崩溃时的最大计费损失。 */
#define COST_FLUSH_MS 1000u
#define COST_FLUSH_OPS 64u

struct cost_tracker {
    pricing_rule_t *rules;
    int rule_count;
    model_cost_t *models;
    airy_mtx_t lock;
    uint32_t pending; /* 自上次成功落盘以来的累计次数 */
    int dirty;
    uint64_t stamp_ms; /* 上次成功落盘（或创建）的单调毫秒 */
};

/* 规则表自持副本：热重载会释放配置侧的旧规则数组，若此处只拷结构体，
 * model_pattern 就成了悬垂指针。深度拷贝让 tracker 的规则生命周期与
 * 配置解耦，谁创建谁释放。 */
static void rules_drop(pricing_rule_t *rules, int count);

static pricing_rule_t *rules_dup(const pricing_rule_t *rules, int count)
{
    if (!rules || count <= 0)
        return NULL;
    pricing_rule_t *dup = (pricing_rule_t *)AIRY_CALLOC((size_t)count, sizeof(pricing_rule_t));
    if (!dup)
        return NULL;
    for (int i = 0; i < count; ++i) {
        dup[i].input_price_per_k = rules[i].input_price_per_k;
        dup[i].output_price_per_k = rules[i].output_price_per_k;
        if (rules[i].model_pattern) {
            dup[i].model_pattern = AIRY_STRDUP(rules[i].model_pattern);
            if (!dup[i].model_pattern) {
                rules_drop(dup, count);
                return NULL;
            }
        }
    }
    return dup;
}

static void rules_drop(pricing_rule_t *rules, int count)
{
    if (!rules)
        return;
    for (int i = 0; i < count; ++i)
        AIRY_FREE((void *)rules[i].model_pattern);
    AIRY_FREE(rules);
}

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

void cost_tracker_price(const pricing_rule_t *rules, int rule_count, const char *model,
                        double *input_price, double *output_price)
{
    *input_price = 0.001;
    *output_price = 0.002;
    for (int i = 0; i < rule_count; ++i) {
        if (match_rule(model, &rules[i])) {
            *input_price = rules[i].input_price_per_k;
            *output_price = rules[i].output_price_per_k;
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
        ct->rules = rules_dup(rules, rule_count);
        if (!ct->rules) {
            AIRY_FREE(ct);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }
        ct->rule_count = rule_count;
    }
    airy_mtx_init(&ct->lock);
    ct->stamp_ms = airy_time_ms();
    return ct;
}

int cost_set_rules(cost_tracker_t *ct, const pricing_rule_t *rules, int rule_count)
{
    if (!ct)
        return -1;

    pricing_rule_t *dup = NULL;
    if (rule_count > 0) {
        dup = rules_dup(rules, rule_count);
        if (!dup)
            return -1;
    }

    airy_mtx_lock(&ct->lock);
    pricing_rule_t *old = ct->rules;
    int old_n = ct->rule_count;
    ct->rules = dup;
    ct->rule_count = rule_count > 0 ? rule_count : 0;
    airy_mtx_unlock(&ct->lock);

    /* 锁外释放：换表后旧表不再被任何读者引用，避免在锁内做长尾释放。 */
    rules_drop(old, old_n);
    return 0;
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
    rules_drop(ct->rules, ct->rule_count);
    AIRY_FREE(ct);
}

void cost_tracker_add(cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                      uint32_t completion_tokens, uint32_t cached_tokens)
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
    m->cached_tokens += cached_tokens;

    double in_price, out_price;
    cost_tracker_price(ct->rules, ct->rule_count, model, &in_price, &out_price);
    m->cost_usd += (prompt_tokens / 1000.0) * in_price + (completion_tokens / 1000.0) * out_price;
    ++ct->pending;
    ct->dirty = 1;
    airy_mtx_unlock(&ct->lock);
}

double cost_tracker_estimate(const cost_tracker_t *ct, const char *model, uint32_t prompt_tokens,
                             uint32_t completion_tokens)
{
    if (!ct || !model)
        return 0.0;
    /* 规则表可被 set_rules 换掉，读侧必须与写侧同锁；const 语义下用
     * 显式转换加锁，与 config_unified 的既有做法一致。 */
    airy_mtx_t *lock = (airy_mtx_t *)&ct->lock;
    airy_mtx_lock(lock);
    double in_price, out_price;
    cost_tracker_price(ct->rules, ct->rule_count, model, &in_price, &out_price);
    airy_mtx_unlock(lock);
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
        cJSON_AddNumberToObject(obj, "cached_tokens", m->cached_tokens);
        cJSON_AddNumberToObject(obj, "cost_usd", m->cost_usd);
        cJSON_AddItemToArray(arr, obj);
        m = m->next;
    }
    cJSON_AddItemToObject(root, "models", arr);
    airy_mtx_unlock(&ct->lock);
    return root;
}

/* 原子写（临时文件 + rename 防半写文件）。sync=0 只刷页缓存：进程崩溃
 * （含 SIGKILL）不丢，断电丢失由时间窗口的同步写兜底；sync=1 为强同步。
 * 落盘成功才清计数：写失败保持 dirty，下一次触发重试。 */
static int cost_write(cost_tracker_t *ct, const char *path, int sync)
{
    /* 快照待写次数：导出到 rename 之间到达的新累计不得被清零，否则丢计费。 */
    airy_mtx_lock(&ct->lock);
    uint32_t snap = ct->pending;
    airy_mtx_unlock(&ct->lock);

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
    if (wok && sync) {
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
    if (airy_file_rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }

    airy_mtx_lock(&ct->lock);
    ct->pending = ct->pending > snap ? ct->pending - snap : 0;
    /* 非同步写只把内容刷新进页缓存，dirty 保持到下一个时间窗口的同步写。 */
    if (sync) {
        ct->dirty = ct->pending > 0;
        ct->stamp_ms = airy_time_ms();
    }
    airy_mtx_unlock(&ct->lock);
    return 0;
}

/* save 无条件同步落盘：仅供进程退出等收尾路径使用。 */
int cost_tracker_save(cost_tracker_t *ct, const char *path)
{
    if (!ct || !path || !path[0])
        return -1;
    return cost_write(ct, path, 1);
}

int cost_tracker_flush(cost_tracker_t *ct, const char *path)
{
    if (!ct || !path || !path[0])
        return -1;

    airy_mtx_lock(&ct->lock);
    uint32_t pending = ct->pending;
    int dirty = ct->dirty;
    uint64_t stamp = ct->stamp_ms;
    airy_mtx_unlock(&ct->lock);

    if (!dirty)
        return 0;

    /* 时间窗口到期做强同步；窗口内只按次数做页缓存刷新，稳态下请求路径
     * 每 64 次才一次短写，且不产生 fsync。 */
    int sync = (airy_time_ms() - stamp) >= COST_FLUSH_MS;
    if (!sync && pending < COST_FLUSH_OPS)
        return 0;
    return cost_write(ct, path, sync);
}

/* 读入整个文件：0 = 已取到内容，1 = 无文件或空文件（非错误，静默跳过），
 * -1 = 读取失败。16MiB 上限防畸形文件把内存打爆。 */
static int file_slurp(const char *path, char **out_buf)
{
    *out_buf = NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;

    long sz = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (sz == 0) {
        fclose(f);
        return 1;
    }
    if (sz > (long)(16 * 1024 * 1024)) {
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
    *out_buf = buf;
    return 0;
}

/* 锁内按模型名查找账目，缺失则头插新建（调用方已持锁）。 */
static model_cost_t *model_get(cost_tracker_t *ct, const char *name)
{
    for (model_cost_t *m = ct->models; m; m = m->next) {
        if (strcmp(m->model, name) == 0)
            return m;
    }

    model_cost_t *m = (model_cost_t *)AIRY_CALLOC(1, sizeof(model_cost_t));
    if (!m)
        return NULL;
    m->model = AIRY_STRDUP(name);
    m->next = ct->models;
    ct->models = m;
    return m;
}

static uint32_t json_u32(const cJSON *item, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(item, key);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

/* 单条历史账目回填：累加而非覆盖，故重复 load 不会抹掉已记的用量。 */
static void entry_apply(cost_tracker_t *ct, const cJSON *item)
{
    const cJSON *mn = cJSON_GetObjectItem(item, "model");
    if (!cJSON_IsString(mn) || !mn->valuestring || !mn->valuestring[0])
        return;

    model_cost_t *m = model_get(ct, mn->valuestring);
    if (!m)
        return;

    const cJSON *cost = cJSON_GetObjectItem(item, "cost_usd");
    m->prompt_tokens += json_u32(item, "prompt_tokens");
    m->completion_tokens += json_u32(item, "completion_tokens");
    m->cached_tokens += json_u32(item, "cached_tokens");
    m->cost_usd += cJSON_IsNumber(cost) ? cost->valuedouble : 0.0;
}

int cost_tracker_load(cost_tracker_t *ct, const char *path)
{
    if (!ct || !path || !path[0])
        return -1;

    char *buf = NULL;
    int rc = file_slurp(path, &buf);
    if (rc != 0)
        return rc > 0 ? 0 : -1;

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
        entry_apply(ct, item);
    }
    airy_mtx_unlock(&ct->lock);
    cJSON_Delete(root);
    return 0;
}
