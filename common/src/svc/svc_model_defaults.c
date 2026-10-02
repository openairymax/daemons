// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_model_defaults.c
 * @brief model.yaml 缺省模型抽取：表驱动单一扫描器（机制）+ 公共抽取入口（策略）。
 *
 * 机制与策略分离：libyaml 事件流骨架（fopen / 初始化 / 事件循环 / 清理）
 * 与字段分发收敛为单一机制件（svc_scan_*），llm / think / global+顶层键
 * 三种抽取策略退化为字段表 + 薄壳。models 表首条目扫描结构不同（序列 +
 * 首条目 + 提前退出），保留独立循环但复用字段分发与开关件。
 *
 * 段式扫描状态机（svc_scan_run）：
 *   - pending：段键已见，等待其 mapping 展开
 *   - depth：段内嵌套深度（段自身为 1），仅 depth==1 的键值对进字段表
 *   - top：顶层键模式（v2 表格格式 2026-08-26：顶层 default_model /
 *     default_provider 与 global 段等价）；顶层键捕获不设 dest 门，
 *     段内捕获以 dest 非空为门（保持既有可观测语义）
 *
 * 返回契约（与头文件一致）：缺字段留空非错；无段保持 out 初值非错；
 * parse 中途失败仍算成功；models0 无表返回 AIRY_ERR_NOT_FOUND。
 */

#include "svc_model_defaults.h"

#include "error.h"
#include "platform_paths.h"
#include "airy_memory_inline.h"

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

#ifdef HAVE_YAML
#include <yaml.h>
#endif

/* 字段种类：STR 定长串；TOKENS 走 svc_tokens_parse；BOOL 走 svc_yaml_bool；
 * U32 走 strtol（正数才写）。dest 用直接指针，兼容双缓冲与两 struct 三种
 * 输出形态。 */
enum {
    SVC_FLD_STR,
    SVC_FLD_TOKENS,
    SVC_FLD_BOOL,
    SVC_FLD_U32,
};

typedef struct {
    const char *name;
    int kind;
    void *dest;
    size_t cap;
} svc_yaml_field_t;

/* 段式扫描策略：section 为段名；sec/sec_n 为段内字段表；top/top_n 为
 * 顶层键字段表（NULL = 无顶层键模式）。 */
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

#ifdef HAVE_YAML

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

static const svc_yaml_field_t *svc_fld_match(const svc_yaml_field_t *tbl, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(key, tbl[i].name) == 0)
            return &tbl[i];
    return NULL;
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
    default:
        break;
    }
}

static void svc_fld_apply(const svc_yaml_field_t *tbl, int n, const char *key, const char *val)
{
    const svc_yaml_field_t *f = svc_fld_match(tbl, n, key);
    if (f)
        svc_fld_set(f, val);
}

static int svc_yaml_open(const char *path, FILE **out_f, yaml_parser_t *parser)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return AIRY_ERR_IO;
    if (!yaml_parser_initialize(parser)) {
        fclose(f);
        return AIRY_ERR_NOT_SUPPORTED;
    }
    yaml_parser_set_input_file(parser, f);
    *out_f = f;
    return 0;
}

static void svc_yaml_close(FILE *f, yaml_parser_t *parser)
{
    yaml_parser_delete(parser);
    fclose(f);
}

typedef struct {
    int pending;
    int depth;
    int top;
    char key[128];
} svc_yaml_scan_t;

static void svc_scan_scalar(svc_yaml_scan_t *st, const svc_scan_cfg_t *cfg, const char *val)
{
    const svc_yaml_field_t *f;

    if (!val)
        return;
    if (st->pending) {
        /* 段键后跟标量：段未展开，丢弃该值 */
        st->pending = 0;
        st->key[0] = '\0';
    } else if (st->depth > 0) {
        if (st->depth == 1 && st->key[0] == '\0') {
            f = svc_fld_match(cfg->sec, cfg->sec_n, val);
            if (f && f->dest)
                AIRY_STRNCPY_TERM(st->key, val, sizeof(st->key));
        } else if (st->depth == 1) {
            svc_fld_apply(cfg->sec, cfg->sec_n, st->key, val);
            st->key[0] = '\0';
        }
    } else if (st->top) {
        svc_fld_apply(cfg->top, cfg->top_n, st->key, val);
        st->top = 0;
        st->key[0] = '\0';
    } else if (strcmp(val, cfg->section) == 0) {
        st->pending = 1;
    } else if (cfg->top) {
        f = svc_fld_match(cfg->top, cfg->top_n, val);
        if (f) {
            AIRY_STRNCPY_TERM(st->key, val, sizeof(st->key));
            st->top = 1;
        }
    }
}

static void svc_scan_run(yaml_parser_t *parser, const svc_scan_cfg_t *cfg)
{
    svc_yaml_scan_t st = {0};
    yaml_event_t ev;
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(parser, &ev))
            break;
        switch (ev.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            if (st.pending) {
                st.depth = 1;
                st.pending = 0;
            } else if (st.depth > 0) {
                st.depth++;
                st.key[0] = '\0';
            }
            break;
        case YAML_MAPPING_END_EVENT:
            if (st.depth > 0) {
                st.depth--;
                st.key[0] = '\0';
            }
            break;
        case YAML_SCALAR_EVENT:
            svc_scan_scalar(&st, cfg, (const char *)ev.data.scalar.value);
            break;
        default:
            break;
        }
        yaml_event_delete(&ev);
    }
}

#endif /* HAVE_YAML */

static int svc_scan_file(const char *path, const svc_scan_cfg_t *cfg)
{
#ifndef HAVE_YAML
    (void)path;
    (void)cfg;
    return AIRY_ERR_NOT_SUPPORTED;
#else
    FILE *f;
    yaml_parser_t parser;
    int rc = svc_yaml_open(path, &f, &parser);
    if (rc)
        return rc;
    svc_scan_run(&parser, cfg);
    svc_yaml_close(f, &parser);
    return 0;
#endif
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
    svc_scan_cfg_t cfg = { "global", tbl, 2, tbl, 2 };
    return svc_scan_file(path, &cfg);
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
    if (!out_model[0])
        AIRY_STRNCPY_TERM(out_model, SVC_MODEL_DEFAULT_FALLBACK, model_sz);

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

    return 0;
}

int svc_model_defaults_llm_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

    svc_yaml_field_t tbl[] = {
        { "api_format",  SVC_FLD_STR,    out->api_format,        sizeof(out->api_format)  },
        { "base_url",    SVC_FLD_STR,    out->base_url,          sizeof(out->base_url)    },
        { "api_key_env", SVC_FLD_STR,    out->api_key_env,       sizeof(out->api_key_env) },
        { "model",       SVC_FLD_STR,    out->model,             sizeof(out->model)       },
        { "max_output",  SVC_FLD_TOKENS, &out->max_output_tokens, 0                       },
    };
    svc_scan_cfg_t cfg = { "llm", tbl, 5, NULL, 0 };
    return svc_scan_file(path, &cfg);
}

#ifdef HAVE_YAML

/* models 表首条目扫描状态：in_models 已进入 models 序列；item_depth 为
 * 条目 mapping 深度；have_key 为键值交替状态；captured 为首条目收毕。 */
typedef struct {
    int in_models;
    int item_depth;
    int have_key;
    int captured;
    char key[128];
} svc_m0_scan_t;

static void svc_m0_depth(svc_m0_scan_t *st, int start)
{
    if (!st->in_models)
        return;
    if (start) {
        st->item_depth++;
        if (st->item_depth == 1)
            st->have_key = 0;
    } else {
        st->item_depth--;
        if (st->item_depth == 0)
            st->captured = 1;
    }
}

static void svc_m0_scalar(svc_m0_scan_t *st, const svc_yaml_field_t *tbl, int n, const char *val)
{
    if (!val)
        return;
    if (!st->in_models && st->item_depth == 0 && st->have_key == 0) {
        if (strcmp(val, "models") == 0)
            st->in_models = 1;
    } else if (st->in_models && st->item_depth == 1) {
        if (!st->have_key) {
            AIRY_STRNCPY_TERM(st->key, val, sizeof(st->key));
            st->have_key = 1;
        } else {
            svc_fld_apply(tbl, n, st->key, val);
            st->have_key = 0;
        }
    }
}

#endif /* HAVE_YAML */

/* v2 表格格式（2026-08-26）：读取顶层 models 表首个条目的主连接字段
 * （api_format / base_url / api_key_env / model_id / max_output），供
 * llm_d / gateway_d 在 llm: 段缺省时取默认连接。models[0].model_id 即
 * 默认模型名。 */
int svc_model_defaults_models0_from_yaml(const char *path, svc_model_llm_config_t *out)
{
    if (!path || !path[0] || !out)
        return AIRY_ERR_INVALID_PARAM;

#ifndef HAVE_YAML
    return AIRY_ERR_NOT_SUPPORTED;
#else
    svc_yaml_field_t tbl[] = {
        { "api_format",  SVC_FLD_STR,    out->api_format,        sizeof(out->api_format)  },
        { "base_url",    SVC_FLD_STR,    out->base_url,          sizeof(out->base_url)    },
        { "api_key_env", SVC_FLD_STR,    out->api_key_env,       sizeof(out->api_key_env) },
        { "model_id",    SVC_FLD_STR,    out->model,             sizeof(out->model)       },
        { "max_output",  SVC_FLD_TOKENS, &out->max_output_tokens, 0                       },
    };
    svc_m0_scan_t st = {0};
    FILE *f;
    yaml_parser_t parser;
    yaml_event_t ev;
    int rc = svc_yaml_open(path, &f, &parser);
    int done = 0;

    if (rc)
        return rc;

    while (!done && !st.captured) {
        if (!yaml_parser_parse(&parser, &ev))
            break;
        switch (ev.type) {
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            svc_m0_depth(&st, 1);
            break;
        case YAML_MAPPING_END_EVENT:
            svc_m0_depth(&st, 0);
            break;
        case YAML_SCALAR_EVENT:
            svc_m0_scalar(&st, tbl, 5, (const char *)ev.data.scalar.value);
            break;
        default:
            break;
        }
        yaml_event_delete(&ev);
    }

    svc_yaml_close(f, &parser);
    return st.captured ? 0 : AIRY_ERR_NOT_FOUND;
#endif
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
