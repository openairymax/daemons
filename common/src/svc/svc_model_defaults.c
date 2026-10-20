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

/* 装载文档并按策略扫描：顶层键表扫 root（v2 表格格式顶层 default_model /
 * default_provider / performance_mode 与段内等价），段表扫 section mapping
 * 的直接条目。yaml_parse_file 的打不开/超限失败统一映射 AIRY_ERR_IO；
 * 空文档（root 非 mapping）等价无字段，返回 0。 */
static int svc_scan_file(const char *path, const svc_scan_cfg_t *cfg)
{
    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_ERR_IO;
    if (yaml_parse_file(doc, path) != 0) {
        yaml_destroy(doc);
        return AIRY_ERR_IO;
    }

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

int svc_model_defaults_resolve(const char *base_model, const char *base_provider, char *out_model,
                               size_t model_sz, char *out_provider, size_t prov_sz)
{
    if (!out_model || model_sz == 0)
        return AIRY_ERR_INVALID_PARAM;

    out_model[0] = '\0';
    if (out_provider && prov_sz > 0)
        out_provider[0] = '\0';

    if (base_model && base_model[0])
        AIRY_STRNCPY_TERM(out_model, base_model, model_sz);
    if (base_provider && base_provider[0] && out_provider && prov_sz > 0)
        AIRY_STRNCPY_TERM(out_provider, base_provider, prov_sz);

    const char *cfg_dir = airy_config_dir();
    if (cfg_dir && cfg_dir[0]) {
        char user_path[1024];
        int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
        if (plen > 0 && plen < (int)sizeof(user_path))
            svc_user_model_cfg(user_path, out_model, model_sz, out_provider, prov_sz);
    }

    const char *env_model = getenv("AIRY_AGENT_MODEL");
    if (env_model && env_model[0])
        AIRY_STRNCPY_TERM(out_model, env_model, model_sz);

    /* 模型名属部署策略：机制核不内置厂商兜底。无任何来源给出模型即
     * fail-closed，由调用方显式报配置缺失，而不是用隐式默认模型掩盖。 */
    return out_model[0] ? 0 : AIRY_ERR_NOT_FOUND;
}

/* llm 段字段表：fill 供节点树消费者复用（llm_d expand 路径免二次解析），
 * llm_from_yaml 经段扫描走同一张表。 */
int svc_model_llm_fill(struct yaml_node *llm_section, svc_model_llm_config_t *out)
{
    if (!out)
        return AIRY_ERR_INVALID_PARAM;
    if (!llm_section || llm_section->type != YAML_NODE_MAPPING)
        return AIRY_ERR_NOT_FOUND;

    svc_yaml_field_t tbl[] = {
        { "api_format",    SVC_FLD_STR,    out->api_format,        sizeof(out->api_format)    },
        { "base_url",      SVC_FLD_STR,    out->base_url,          sizeof(out->base_url)      },
        { "api_key_env",   SVC_FLD_STR,    out->api_key_env,       sizeof(out->api_key_env)   },
        { "model",         SVC_FLD_STR,    out->model,             sizeof(out->model)         },
        { "max_output",    SVC_FLD_TOKENS, &out->max_output_tokens, 0                         },
        { "extra_headers", SVC_FLD_STR,    out->extra_headers,     sizeof(out->extra_headers) },
    };
    svc_scan_map(llm_section, tbl, 6);
    return 0;
}

int svc_model_defaults_llm_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_ERR_IO;
    if (yaml_parse_file(doc, path) != 0) {
        yaml_destroy(doc);
        return AIRY_ERR_IO;
    }

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

    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_ERR_IO;
    if (yaml_parse_file(doc, path) != 0) {
        yaml_destroy(doc);
        return AIRY_ERR_IO;
    }

    int rc = AIRY_ERR_NOT_FOUND;
    struct yaml_node *models = yaml_get(yaml_root(doc), "models");
    if (models && models->type == YAML_NODE_SEQUENCE && models->sequence.count > 0) {
        struct yaml_node *first = yaml_get_index(models, 0);
        if (first && first->type == YAML_NODE_MAPPING) {
            svc_yaml_field_t tbl[] = {
                { "api_format",    SVC_FLD_STR,    out->api_format,        sizeof(out->api_format)    },
                { "base_url",      SVC_FLD_STR,    out->base_url,          sizeof(out->base_url)      },
                { "api_key_env",   SVC_FLD_STR,    out->api_key_env,       sizeof(out->api_key_env)   },
                { "model_id",      SVC_FLD_STR,    out->model,             sizeof(out->model)         },
                { "max_output",    SVC_FLD_TOKENS, &out->max_output_tokens, 0                         },
                { "extra_headers", SVC_FLD_STR,    out->extra_headers,     sizeof(out->extra_headers) },
            };
            svc_scan_map(first, tbl, 6);
            rc = 0;
        }
    }
    yaml_destroy(doc);
    return rc;
}

int svc_model_defaults_think_from_yaml(const char *path, svc_model_think_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

    svc_yaml_field_t tbl[] = {
        { "enabled",           SVC_FLD_BOOL, &out->enabled, 0 },
        { "think2_slow_model", SVC_FLD_STR,  out->think2_slow_model, sizeof(out->think2_slow_model) },
        { "think1_fast_model", SVC_FLD_STR,  out->think1_fast_model, sizeof(out->think1_fast_model) },
        { "think1_prof_model", SVC_FLD_STR,  out->think1_prof_model, sizeof(out->think1_prof_model) },
        { "timeout_ms",        SVC_FLD_U32,  &out->timeout_ms, 0 },
    };
    svc_scan_cfg_t cfg = { "think", tbl, 5, NULL, 0 };
    return svc_scan_file(path, &cfg);
}
