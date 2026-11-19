// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_model_defaults.c
 * @brief model.yaml 缺省模型抽取：表驱动单一扫描器（机制）+ 公共抽取入口（策略）。
 *
 * 机制与策略分离：commons yaml_minimal 节点树装载（yaml_create /
 * yaml_parse_file / yaml_destroy）与字段分发收敛为单一机制件（svc_scan_*），
 * llm / think / global+顶层键 / performance_mode 四种抽取策略退化为字段表 +
 * 薄壳。models 表首条目结构不同（序列 + 首条目），保留独立 walk 但复用
 * 字段分发与开关件。全平台无 libyaml 依赖（B13：声明面只保留一个 YAML
 * 装载器，Windows 无 libyaml 构建同样工作）。
 *
 * 反转遍历（svc_scan_map）：yaml_minimal 的 mapping 不暴露按键序取键
 * API，故由字段表逐项向文档 yaml_get 查询，命中的直接 SCALAR 值按字段
 * 种类写 dest。写结果集合与正向事件流遍历等价，代码更短。
 *
 * 返回契约（与头文件一致）：缺字段留空非错；无段保持 out 初值非错；
 * 文件打不开/解析失败返回 AIRY_ERR_IO；models0 无表返回 AIRY_ERR_NOT_FOUND。
 */

#include "svc_model_defaults.h"

#include "error.h"
#include "io.h"
#include "platform_paths.h"
#include "airy_memory_inline.h"
#include "yaml_minimal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* MSVC 无 <strings.h>（C1083）且无 strcasecmp：映射为 _stricmp */
#if defined(_WIN32)
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/* 字段种类：STR 定长串；TOKENS 走 svc_tokens_parse；BOOL 走 svc_yaml_bool；
 * U32 走 strtol（正数才写）。dest 用直接指针，兼容双缓冲与两 struct 三种
 * 输出形态。 */
enum {
    SVC_FLD_STR,
    SVC_FLD_TOKENS,
    SVC_FLD_BOOL,
    SVC_FLD_U32,
    SVC_FLD_PERF,
};

typedef struct {
    const char *name;
    int kind;
    void *dest;
    size_t cap;
} svc_yaml_field_t;

/* 段式扫描策略：section 为段名（空串 = 仅顶层键模式）；sec/sec_n 为段内
 * 字段表；top/top_n 为顶层键字段表（NULL = 无顶层键模式）。 */
typedef struct {
    const char *section;
    const svc_yaml_field_t *sec;
    int sec_n;
    const svc_yaml_field_t *top;
    int top_n;
} svc_scan_cfg_t;

int svc_tokens_parse(const char *text)
{
    if (!text || !text[0])
        return 0;

    char *end = NULL;
    long base = strtol(text, &end, 10);
    if (end == text || base <= 0)
        return 0;

    long mult = 1;
    if (*end) {
        char suffix = (char)toupper((unsigned char)*end);
        if (suffix == 'K')
            mult = 1024;
        else if (suffix == 'M')
            mult = 1024L * 1024L;
        else
            return 0;
        ++end;
    }
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end)
        return 0;

    long long total = (long long)base * mult;
    if (total > INT_MAX)
        return INT_MAX;
    return (int)total;
}

/* performance_mode 字面量：full / lightweight / auto。未知值与空值一律
 * 落 full——轻量路径只应由显式声明触发，绝不由可疑取值推断。 */
static svc_perf_mode_t svc_perf_parse(const char *val)
{
    if (val && strcasecmp(val, "lightweight") == 0)
        return SVC_PERF_LIGHTWEIGHT;
    if (val && strcasecmp(val, "auto") == 0)
        return SVC_PERF_AUTO;
    return SVC_PERF_FULL;
}

/* 布尔字面量：true/yes/on/1 与 false/no/off/0（大小写不敏感）；未知值
 * 返回 -1 不落字段。不可用 yaml_as_bool 替代：未知值必须与 false 区分。 */
static int svc_yaml_bool(const char *val)
{
    if (!val || !*val)
        return -1;
    if (strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0 ||
        strcmp(val, "1") == 0)
        return 1;
    if (strcasecmp(val, "false") == 0 || strcasecmp(val, "no") == 0 ||
        strcasecmp(val, "off") == 0 || strcmp(val, "0") == 0)
        return 0;
    return -1;
}

static void svc_fld_set(const svc_yaml_field_t *f, const char *val)
{
    switch (f->kind) {
    case SVC_FLD_STR:
        if (f->cap > 0)
            AIRY_STRNCPY_TERM(f->dest, val, f->cap);
        break;
    case SVC_FLD_TOKENS: {
        int n = svc_tokens_parse(val);
        if (n > 0)
            *(int *)f->dest = n;
        break;
    }
    case SVC_FLD_BOOL: {
        int b = svc_yaml_bool(val);
        if (b >= 0)
            *(int *)f->dest = b;
        break;
    }
    case SVC_FLD_U32: {
        long t = strtol(val, NULL, 10);
        if (t > 0)
            *(uint32_t *)f->dest = (uint32_t)t;
        break;
    }
    case SVC_FLD_PERF:
        *(svc_perf_mode_t *)f->dest = svc_perf_parse(val);
        break;
    default:
        break;
    }
}

/* 反转遍历：字段表逐项向 mapping 查询。仅消费直接 SCALAR 值条目——嵌套
 * 子段（如 global.default_retry）不展开，与原 depth==1 语义一致。 */
static void svc_scan_map(struct yaml_node *map, const svc_yaml_field_t *tbl, int n)
{
    for (int i = 0; i < n; i++) {
        if (!tbl[i].dest)
            continue;
        struct yaml_node *v = yaml_get(map, tbl[i].name);
        if (v && v->type == YAML_NODE_SCALAR && v->scalar.value)
            svc_fld_set(&tbl[i], v->scalar.value);
    }
}

/* 打开并解析 yaml 文档；失败返回 NULL，调用方无需清理。 */
static yaml_document_t *yaml_open(const char *path)
{
    yaml_document_t *doc = yaml_create();
    if (!doc)
        return NULL;
    if (yaml_parse_file(doc, path) != 0) {
        yaml_destroy(doc);
        return NULL;
    }
    return doc;
}

/* 装载文档并按策略扫描：顶层键表扫 root（v2 表格格式顶层 default_model /
 * default_provider / performance_mode 与段内等价），段表扫 section mapping
 * 的直接条目。yaml_parse_file 的打不开/超限失败统一映射 AIRY_ERR_IO；
 * 空文档（root 非 mapping）等价无字段，返回 0。 */
static int svc_scan_file(const char *path, const svc_scan_cfg_t *cfg)
{
    yaml_document_t *doc = yaml_open(path);
    if (!doc)
        return AIRY_ERR_IO;

    struct yaml_node *root = yaml_root(doc);
    if (root && root->type == YAML_NODE_MAPPING) {
        if (cfg->top)
            svc_scan_map(root, cfg->top, cfg->top_n);
        if (cfg->section[0]) {
            struct yaml_node *sec = yaml_get(root, cfg->section);
            if (sec && sec->type == YAML_NODE_MAPPING)
                svc_scan_map(sec, cfg->sec, cfg->sec_n);
        }
    }
    yaml_destroy(doc);
    return 0;
}

int svc_model_defaults_from_yaml(const char *path, char *out_model, size_t model_sz,
                                 char *out_provider, size_t prov_sz)
{
    if (!path || !path[0])
        return AIRY_ERR_INVALID_PARAM;
    if (out_model && model_sz > 0)
        out_model[0] = '\0';
    if (out_provider && prov_sz > 0)
        out_provider[0] = '\0';

    svc_yaml_field_t tbl[] = {
        { "default_model",    SVC_FLD_STR, out_model,    model_sz },
        { "default_provider", SVC_FLD_STR, out_provider, prov_sz  },
    };
    /* 顶层键与 global 段同表：v2 表格格式两者等价 */
    svc_scan_cfg_t cfg = { "global", tbl, 2, tbl, 2 };
    return svc_scan_file(path, &cfg);
}

svc_perf_mode_t svc_perf_from_yaml(const char *path)
{
    svc_perf_mode_t mode = SVC_PERF_FULL;

    if (!path || !path[0])
        return mode;

    svc_yaml_field_t tbl[] = {
        { "performance_mode", SVC_FLD_PERF, &mode, 0 },
    };
    /* 空 section：仅顶层键模式（performance_mode 是顶层标量键） */
    svc_scan_cfg_t cfg = { "", tbl, 0, tbl, 1 };
    (void)svc_scan_file(path, &cfg);
    return mode;
}

svc_perf_mode_t svc_perf_resolve(const char *path)
{
    const char *env = getenv("AIRY_PERF_MODE");
    if (env && env[0])
        return svc_perf_parse(env);
    return svc_perf_from_yaml(path);
}

/* 用户覆盖文件的模型/供应方取值：default_model，缺省回退 llm.model，
 * 再缺省回退 models[0].model_id（与 llm_d 的展开顺序一致）。 */
static int svc_user_model_cfg(const char *user_path, char *out_model, size_t model_sz,
                              char *out_provider, size_t prov_sz)
{
    char um[128] = {0};
    char up[64] = {0};

    if (svc_model_defaults_from_yaml(user_path, um, sizeof(um), up, sizeof(up)) != 0 || !um[0]) {
        svc_model_llm_config_t llm_cfg;
        AIRY_MEMSET(&llm_cfg, 0, sizeof(llm_cfg));
        if (svc_model_defaults_llm_from_yaml(user_path, &llm_cfg) == 0 && llm_cfg.model[0]) {
            AIRY_STRNCPY_TERM(um, llm_cfg.model, sizeof(um));
        } else {
            AIRY_MEMSET(&llm_cfg, 0, sizeof(llm_cfg));
            if (svc_model_defaults_models0_from_yaml(user_path, &llm_cfg) == 0 && llm_cfg.model[0])
                AIRY_STRNCPY_TERM(um, llm_cfg.model, sizeof(um));
        }
    }

    if (um[0] && out_model && model_sz > 0)
        AIRY_STRNCPY_TERM(out_model, um, model_sz);
    if (up[0] && out_provider && prov_sz > 0)
        AIRY_STRNCPY_TERM(out_provider, up, prov_sz);

    return (um[0] || up[0]) ? 0 : AIRY_ERR_NOT_FOUND;
}

/* 显式入参优先于配置文件：调用方给出的 base 即本次请求的锚点。 */
static void resolve_seed(const char *base_model, const char *base_provider, char *out_model,
                         size_t model_sz, char *out_provider, size_t prov_sz)
{
    if (base_model && base_model[0])
        AIRY_STRNCPY_TERM(out_model, base_model, model_sz);
    if (base_provider && base_provider[0] && out_provider && prov_sz > 0)
        AIRY_STRNCPY_TERM(out_provider, base_provider, prov_sz);
}

/* 用户配置文件覆盖：路径拼接失败即跳过，不改变已锚定的值。 */
static void resolve_user_cfg(char *out_model, size_t model_sz, char *out_provider, size_t prov_sz)
{
    const char *cfg_dir = airy_config_dir();
    if (!cfg_dir || !cfg_dir[0])
        return;
    char user_path[1024];
    int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
    if (plen > 0 && plen < (int)sizeof(user_path))
        svc_user_model_cfg(user_path, out_model, model_sz, out_provider, prov_sz);
}

int svc_model_defaults_resolve(const char *base_model, const char *base_provider, char *out_model,
                               size_t model_sz, char *out_provider, size_t prov_sz)
{
    if (!out_model || model_sz == 0)
        return AIRY_ERR_INVALID_PARAM;

    out_model[0] = '\0';
    if (out_provider && prov_sz > 0)
        out_provider[0] = '\0';

    resolve_seed(base_model, base_provider, out_model, model_sz, out_provider, prov_sz);
    resolve_user_cfg(out_model, model_sz, out_provider, prov_sz);

    const char *env_model = getenv("AIRY_AGENT_MODEL");
    if (env_model && env_model[0])
        AIRY_STRNCPY_TERM(out_model, env_model, model_sz);

    /* 模型名属部署策略：机制核不内置厂商兜底。无任何来源给出模型即
     * fail-closed，由调用方显式报配置缺失，而不是用隐式默认模型掩盖。 */
    return out_model[0] ? 0 : AIRY_ERR_NOT_FOUND;
}

/* llm 段字段表：fill 与 models[0] 两条读取路径共用同一张表，仅默认模型名
 * 的键不同（llm: 段用 model，models: 表用 model_id）。 */
static void llm_scan(struct yaml_node *sec, svc_model_llm_config_t *out, const char *model_key)
{
    svc_yaml_field_t tbl[] = {
        { "api_format",    SVC_FLD_STR,    out->api_format,        sizeof(out->api_format)    },
        { "base_url",      SVC_FLD_STR,    out->base_url,          sizeof(out->base_url)      },
        { "api_key_env",   SVC_FLD_STR,    out->api_key_env,       sizeof(out->api_key_env)   },
        { model_key,       SVC_FLD_STR,    out->model,             sizeof(out->model)         },
        { "max_output",    SVC_FLD_TOKENS, &out->max_output_tokens, 0                         },
        { "extra_headers", SVC_FLD_STR,    out->extra_headers,     sizeof(out->extra_headers) },
    };
    svc_scan_map(sec, tbl, 6);
}

/* llm 段字段表：fill 供节点树消费者复用（llm_d expand 路径免二次解析），
 * llm_from_yaml 经段扫描走同一张表。 */
int svc_model_llm_fill(struct yaml_node *llm_section, svc_model_llm_config_t *out)
{
    if (!out)
        return AIRY_ERR_INVALID_PARAM;
    if (!llm_section || llm_section->type != YAML_NODE_MAPPING)
        return AIRY_ERR_NOT_FOUND;

    llm_scan(llm_section, out, "model");
    return 0;
}

int svc_model_defaults_llm_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

    yaml_document_t *doc = yaml_open(path);
    if (!doc)
        return AIRY_ERR_IO;

    /* 段缺失非错（out 保持初值） */
    int rc = svc_model_llm_fill(yaml_get(yaml_root(doc), "llm"), out);
    yaml_destroy(doc);
    return rc == AIRY_ERR_NOT_FOUND ? 0 : rc;
}

/* v2 表格格式（2026-08-26）：读取顶层 models 表首个条目的主连接字段
 * （api_format / base_url / api_key_env / model_id / max_output），供
 * llm_d / gateway_d 在 llm: 段缺省时取默认连接。models[0].model_id 即
 * 默认模型名。 */
int svc_model_defaults_models0_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

    yaml_document_t *doc = yaml_open(path);
    if (!doc)
        return AIRY_ERR_IO;

    int rc = AIRY_ERR_NOT_FOUND;
    struct yaml_node *models = yaml_get(yaml_root(doc), "models");
    if (models && models->type == YAML_NODE_SEQUENCE && models->sequence.count > 0) {
        struct yaml_node *first = yaml_get_index(models, 0);
        if (first && first->type == YAML_NODE_MAPPING) {
            llm_scan(first, out, "model_id");
            rc = 0;
        }
    }
    yaml_destroy(doc);
    return rc;
}

/* 去重追加：容量满或已存在即丢弃，保持首次出现顺序。 */
static void ids_add(char ids[][SVC_MODEL_ID_MAX], size_t cap, size_t *out_n, const char *id)
{
    if (*out_n >= cap)
        return;
    for (size_t k = 0; k < *out_n; ++k) {
        if (strcmp(ids[k], id) == 0)
            return;
    }
    AIRY_STRNCPY_TERM(ids[(*out_n)++], id, SVC_MODEL_ID_MAX);
}

/* 简化 llm 段优先：存在且非空时 llm_d 只展开这一个 provider。 */
static size_t ids_from_llm(struct yaml_node *root, char ids[][SVC_MODEL_ID_MAX], size_t cap)
{
    if (cap == 0)
        return 0;
    struct yaml_node *llm = yaml_get(root, "llm");
    if (!llm || llm->type != YAML_NODE_MAPPING)
        return 0;
    const char *one = yaml_as_string(yaml_get(llm, "model"), "");
    if (!one || !one[0])
        return 0;
    AIRY_STRNCPY_TERM(ids[0], one, SVC_MODEL_ID_MAX);
    return 1;
}

/* models[] 表逐行取 model_id，与 llm 段结果合并去重。 */
static size_t ids_from_models(struct yaml_node *root, char ids[][SVC_MODEL_ID_MAX], size_t cap,
                              size_t n)
{
    struct yaml_node *models = yaml_get(root, "models");
    if (!models || models->type != YAML_NODE_SEQUENCE)
        return n;
    for (size_t i = 0; i < models->sequence.count && n < cap; ++i) {
        struct yaml_node *item = yaml_get_index(models, i);
        if (!item || item->type != YAML_NODE_MAPPING)
            continue;
        const char *id = yaml_as_string(yaml_get(item, "model_id"), "");
        if (!id || !id[0])
            continue;
        ids_add(ids, cap, &n, id);
    }
    return n;
}

int model_ids_from_yaml(const char *path, char ids[][SVC_MODEL_ID_MAX], size_t cap,
                            size_t *out_n)
{
    if (!path || !path[0] || !ids || cap == 0 || !out_n)
        return AIRY_ERR_INVALID_PARAM;

    *out_n = 0;
    yaml_document_t *doc = yaml_open(path);
    if (!doc)
        return AIRY_ERR_IO;

    struct yaml_node *root = yaml_root(doc);
    size_t n = ids_from_llm(root, ids, cap);
    n = ids_from_models(root, ids, cap, n);
    yaml_destroy(doc);
    *out_n = n;
    return n > 0 ? 0 : AIRY_ERR_NOT_FOUND;
}

/* 槽位表：角色名 / 废弃 think 键 / 新 env / 废弃 env 一一对齐，三处索引
 * 共用 SVC_SLOT_*，避免角色顺序在不同来源之间漂移。 */
static const char *const k_slot_name[SVC_SLOT_N] = { "t2", "t1f", "t1p" };
static const char *const k_slot_legacy[SVC_SLOT_N] = { "think2_slow_model", "think1_fast_model",
                                                       "think1_prof_model" };
static const char *const k_slot_env[SVC_SLOT_N] = { "AIRY_MODEL_T2", "AIRY_MODEL_T1F",
                                                    "AIRY_MODEL_T1P" };
static const char *const k_slot_env_old[SVC_SLOT_N] = { "AIRY_THINK2_SLOW_MODEL",
                                                        "AIRY_THINK1_FAST_MODEL",
                                                        "AIRY_THINK1_PROF_MODEL" };

const char *svc_slot_name(unsigned role)
{
    return role < SVC_SLOT_N ? k_slot_name[role] : NULL;
}

char *svc_slot_buf(svc_model_slots_t *slots, unsigned role)
{
    if (!slots)
        return NULL;
    switch (role) {
    case SVC_SLOT_T2:
        return slots->t2;
    case SVC_SLOT_T1F:
        return slots->t1f;
    case SVC_SLOT_T1P:
        return slots->t1p;
    default:
        return NULL;
    }
}

const char *svc_slot_get(const svc_model_slots_t *slots, unsigned role)
{
    if (!slots || role >= SVC_SLOT_N)
        return NULL;
    return role == SVC_SLOT_T2 ? slots->t2 : role == SVC_SLOT_T1F ? slots->t1f : slots->t1p;
}

int svc_slot_fill(struct yaml_node *root, svc_model_slots_t *out)
{
    if (!out)
        return AIRY_ERR_INVALID_PARAM;
    if (!root || root->type != YAML_NODE_MAPPING)
        return 0;

    struct yaml_node *think = yaml_get(root, "think");
    struct yaml_node *slots = yaml_get(root, "slots");
    if (think && think->type == YAML_NODE_MAPPING) {
        int b = svc_yaml_bool(yaml_as_string(yaml_get(think, "enabled"), NULL));
        if (b >= 0)
            out->enabled = b;
        long t = strtol(yaml_as_string(yaml_get(think, "timeout_ms"), "0"), NULL, 10);
        if (t > 0)
            out->timeout_ms = (uint32_t)t;
    }

    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        char *dst = svc_slot_buf(out, i);
        const char *v = slots ? yaml_as_string(yaml_get(slots, k_slot_name[i]), NULL) : NULL;
        if (v && v[0]) {
            AIRY_STRNCPY_TERM(dst, v, SVC_MODEL_ID_MAX);
            continue;
        }
        const char *lv = think ? yaml_as_string(yaml_get(think, k_slot_legacy[i]), NULL) : NULL;
        if (lv && lv[0]) {
            AIRY_STRNCPY_TERM(dst, lv, SVC_MODEL_ID_MAX);
            out->legacy_mask |= 1u << i;
        }
    }
    return 0;
}

int svc_slot_load(const char *path, svc_model_slots_t *out)
{
    if (!path || !out)
        return AIRY_ERR_INVALID_PARAM;

    yaml_document_t *doc = yaml_open(path);
    if (!doc)
        return AIRY_ERR_IO;
    int rc = svc_slot_fill(yaml_root(doc), out);
    yaml_destroy(doc);
    return rc;
}

int svc_slot_env(unsigned role, char *buf, size_t cap, int *legacy)
{
    if (role >= SVC_SLOT_N || !buf || cap == 0)
        return 0;
    if (legacy)
        *legacy = 0;

    const char *old = getenv(k_slot_env_old[role]);
    const char *v = getenv(k_slot_env[role]);
    int leg = (old && old[0]) ? 1 : 0;
    if (!v || !v[0])
        v = old;
    if (!v || !v[0]) {
        buf[0] = '\0';
        return 0;
    }
    AIRY_STRNCPY_TERM(buf, v, cap);
    if (legacy)
        *legacy = leg;
    return 1;
}

int svc_slot_resolve(const char *path, svc_model_slots_t *out)
{
    if (!out)
        return AIRY_ERR_INVALID_PARAM;

    int rc = 0;
    if (path && path[0])
        rc = svc_slot_load(path, out);

    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        char v[SVC_MODEL_ID_MAX];
        int leg = 0;
        if (!svc_slot_env(i, v, sizeof(v), &leg))
            continue;
        AIRY_STRNCPY_TERM(svc_slot_buf(out, i), v, SVC_MODEL_ID_MAX);
        if (leg)
            out->legacy_mask |= 1u << i;
    }
    return rc;
}

/* 槽位写入（C-19）：只重写顶层 slots: 块中的 t2/t1f/t1p 三行，其余字节
 * 原样保留（注释/顺序/他段不受影响）；缺键在块内就地补齐，整块缺失则
 * 追加。走临时文件 + rename 原子替换，监视线程不会读到半截配置。 */
#define SVC_SLOT_FMAX (512u * 1024u)

static int slot_put(char *o, size_t cap, size_t *off, const char *s, size_t n)
{
    if (*off + n + 1 > cap)
        return AIRY_ERR_IO;
    __builtin_memcpy(o + *off, s, n);
    *off += n;
    o[*off] = '\0';
    return 0;
}

static int slot_key_at(const char *p, const char *e)
{
    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        size_t kl = strlen(k_slot_name[i]);
        if ((size_t)(e - p) < kl + 1 || strncmp(p, k_slot_name[i], kl) != 0 || p[kl] != ':')
            continue;
        char nx = (size_t)(e - p) > kl + 1 ? p[kl + 1] : '\0';
        if (nx == '\0' || nx == ' ' || nx == '\t' || nx == '\r')
            return (int)i;
    }
    return -1;
}

static int slot_line(char *o, size_t cap, size_t *off, unsigned role, const char *val)
{
    char buf[SVC_MODEL_ID_MAX + 32];
    int n = snprintf(buf, sizeof(buf), "  %s: \"%s\"\n", k_slot_name[role], val ? val : "");
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return AIRY_ERR_IO;
    return slot_put(o, cap, off, buf, (size_t)n);
}

static int slot_pad(char *o, size_t cap, size_t *off, unsigned done,
                    const svc_model_slots_t *slots)
{
    for (unsigned i = 0; i < SVC_SLOT_N; ++i) {
        if (done & (1u << i))
            continue;
        int rc = slot_line(o, cap, off, i, svc_slot_get(slots, i));
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* 段头判定：顶层 "slots:" 行（可带注释尾随）。 */
static int slot_is_head(const char *q, size_t trim)
{
    return trim >= 6 && strncmp(q, "slots:", 6) == 0 &&
           (trim == 6 || q[6] == ' ' || q[6] == '\t');
}

/* 重写游标：输出缓冲、已完成槽位与只读输入。 */
typedef struct {
    char *out;
    size_t cap;
    size_t off;
    const svc_model_slots_t *slots;
    unsigned done;
} slot_w_t;

/* 段切换：进入 slots: 段；遇到新的顶层键时补齐剩余槽位并退出该段。 */
static int slot_section(slot_w_t *w, const char *q, size_t trim, int top, int *in_slots,
                        int *found)
{
    if (!top)
        return 0;
    if (slot_is_head(q, trim)) {
        *in_slots = 1;
        *found = 1;
        return 0;
    }
    if (*in_slots && trim > 0) {
        if (slot_pad(w->out, w->cap, &w->off, w->done, w->slots) != 0)
            return -1;
        *in_slots = 0;
    }
    return 0;
}

/* 单行重写：段内命中槽位键则就地替换值，其余行原样搬运。 */
static int slot_walk_line(slot_w_t *w, const char *p, const char *le, const char *next,
                          int *in_slots, int *found)
{
    const char *q = p;
    while (q < le && (*q == ' ' || *q == '\t'))
        q++;
    size_t trim = (size_t)(le - q);
    if (trim > 0 && q[trim - 1] == '\r')
        trim--;
    int top = (q == p);

    if (slot_section(w, q, trim, top, in_slots, found) != 0)
        return -1;
    if (*in_slots && !top) {
        int role = slot_key_at(q, q + trim);
        if (role >= 0) {
            unsigned r = (unsigned)role;
            if (slot_line(w->out, w->cap, &w->off, r, svc_slot_get(w->slots, r)) != 0)
                return -1;
            w->done |= 1u << r;
            return 0;
        }
    }
    return slot_put(w->out, w->cap, &w->off, p, (size_t)(next - p));
}

/* 行切分：返回下一行起点，le 回填本行结束（不含换行）。 */
static const char *slot_next_line(const char *p, const char *end, const char **le)
{
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    *le = nl ? nl : end;
    return nl ? nl + 1 : end;
}

/* 收尾：补齐段尾槽位；全文没有 slots 段时在末尾追加一整段。 */
static int slot_finish(slot_w_t *w, int in_slots, int found)
{
    if (in_slots && slot_pad(w->out, w->cap, &w->off, w->done, w->slots) != 0)
        return -1;
    if (found)
        return 0;
    if (w->off > 0 && w->out[w->off - 1] != '\n' &&
        slot_put(w->out, w->cap, &w->off, "\n", 1) != 0)
        return -1;
    if (slot_put(w->out, w->cap, &w->off, "slots:\n", 7) != 0)
        return -1;
    return slot_pad(w->out, w->cap, &w->off, 0, w->slots);
}

int svc_slot_save(const char *path, const svc_model_slots_t *slots)
{
    if (!path || !path[0] || !slots)
        return AIRY_ERR_INVALID_PARAM;

    char *src = NULL;
    size_t n = 0;
    int rc = airy_io_read_file(path, &src, &n);
    if (rc != 0) {
        /* 目标可缺席：$AIRY_CONFIG_DIR/model.yaml 首次写入时按空文件起，
         * 只落 slots 段，其余配置仍由主配置提供。 */
        AIRY_FREE(src);
        src = NULL;
        n = 0;
    }
    if (n > SVC_SLOT_FMAX) {
        AIRY_FREE(src);
        return AIRY_ERR_IO;
    }

    size_t cap = n + 512;
    char *out = (char *)AIRY_MALLOC(cap);
    if (!out) {
        AIRY_FREE(src);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    out[0] = '\0';

    slot_w_t w = {out, cap, 0, slots, 0};
    int in_slots = 0;
    int found = 0;
    const char *p = src;
    const char *end = src + n;

    while (p < end) {
        const char *le = NULL;
        const char *next = slot_next_line(p, end, &le);
        if (slot_walk_line(&w, p, le, next, &in_slots, &found) != 0)
            goto fail;
        p = next;
    }

    if (slot_finish(&w, in_slots, found) != 0)
        goto fail;

    rc = airy_io_write_file(path, out, w.off);
    AIRY_FREE(out);
    AIRY_FREE(src);
    return rc == 0 ? 0 : AIRY_ERR_IO;

fail:
    AIRY_FREE(out);
    AIRY_FREE(src);
    return AIRY_ERR_IO;
}
