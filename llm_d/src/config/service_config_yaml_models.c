// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml_models.c
 * @brief LLM model.yaml models/providers-section parsing (split from
 *        service_config.c, 2026-08-27): walks the commons yaml_minimal node
 *        tree over the models/providers arrays and expands the simplified
 *        top-level llm section.
 *
 * 解析出的 models[] 与 providers 段声明存入共享的 svc_yaml_state_t，
 * 由 service_config_yaml_providers.c 聚合导出、pricing 件读取价格字段；
 * 状态结构经 config/types.h 共享。装载器为 commons
 * utils/config_unified/yaml_minimal（声明面只保留一个 YAML 装载器，B13），
 * 不再内联 libyaml 事件状态机。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"
#include "yaml_minimal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/types.h"

/* 取映射 item 中 key 对应的标量字符串；键缺失或非标量返回 NULL，与原
 * kv map 未命中语义一致。 */
static const char *svc_yaml_item_str(struct yaml_node *item, const char *key)
{
    return yaml_as_string(yaml_get(item, key), NULL);
}

/* providers 段的一段声明 → st->cur_p，并追加进 st->pcfg[16]。 */
static void svc_yaml_parse_prov(svc_yaml_state_t *st, struct yaml_node *item)
{
    __builtin_memset(&st->cur_p, 0, sizeof(st->cur_p));

    /* 嵌套 models: 序列 → cur_p.model_names。与原状态机一致，收集动作与
     * name 是否存在无关：名称为空时在下方统一释放。 */
    struct yaml_node *mn = yaml_get(item, "models");
    size_t mc = yaml_size(mn);
    for (size_t k = 0; k < mc && st->cur_p.model_count < 64; ++k) {
        const char *mv = yaml_as_string(yaml_get_index(mn, k), NULL);
        if (mv)
            st->cur_p.model_names[st->cur_p.model_count++] = AIRY_STRDUP(mv);
    }

    const char *pn = svc_yaml_item_str(item, "name");
    if (!(pn && pn[0])) {
        for (size_t k = 0; k < st->cur_p.model_count; ++k)
            AIRY_FREE(st->cur_p.model_names[k]);
        st->cur_p.model_count = 0;
        return;
    }

    AIRY_STRNCPY_TERM(st->cur_p.name, pn, sizeof(st->cur_p.name));
    const char *pke = svc_yaml_item_str(item, "api_key_env");
    if (pke)
        AIRY_STRNCPY_TERM(st->cur_p.api_key_env, pke, sizeof(st->cur_p.api_key_env));
    const char *pb = svc_yaml_item_str(item, "base_url");
    if (pb)
        AIRY_STRNCPY_TERM(st->cur_p.base_url, pb, sizeof(st->cur_p.base_url));
    const char *pt = svc_yaml_item_str(item, "timeout_sec");
    if (pt)
        st->cur_p.timeout_sec = (int)strtol(pt, NULL, 10);
    const char *pr = svc_yaml_item_str(item, "max_retries");
    if (pr)
        st->cur_p.max_retries = (int)strtol(pr, NULL, 10);
    const char *pf = svc_yaml_item_str(item, "api_format");
    if (pf)
        AIRY_STRNCPY_TERM(st->cur_p.api_format, pf, sizeof(st->cur_p.api_format));
    const char *px = svc_yaml_item_str(item, "extra_headers");
    if (px)
        AIRY_STRNCPY_TERM(st->cur_p.extra_headers, px, sizeof(st->cur_p.extra_headers));

    if (st->pcfg_count < 16) {
        st->pcfg[st->pcfg_count++] = st->cur_p;
    } else {
        for (size_t k = 0; k < st->cur_p.model_count; ++k)
            AIRY_FREE(st->cur_p.model_names[k]);
        st->cur_p.model_count = 0;
    }
}

/* models[] 行字段表：键 → 字段偏移/容量/伴随标志位/取值类型，顺序无关。 */
enum { YF_STR, YF_INT, YF_TOKENS, YF_PRICE, YF_BOOL };
typedef struct {
    const char *key;
    unsigned off, cap, flag, kind;
} svc_yaml_field_t;
#define YF(k, f, t) { k, offsetof(model_entry_t, f), sizeof(((model_entry_t *)0)->f), 0, t }
#define YF_P(k, f, g) { k, offsetof(model_entry_t, f), sizeof(((model_entry_t *)0)->f), offsetof(model_entry_t, g), YF_PRICE }

static const svc_yaml_field_t svc_yaml_fields[] = {
    YF("mode", mode, YF_STR), YF("api_format", api_format, YF_STR),
    YF("context_window", context_window, YF_STR), YF("thinking", thinking, YF_STR),
    YF("extra_headers", extra_headers, YF_STR), YF("tool_rounds", tool_rounds, YF_INT),
    YF("timeout_sec", timeout_sec, YF_INT), YF("max_retries", max_retries, YF_INT),
    YF("max_output", max_output_tokens, YF_TOKENS), YF("vision", vision, YF_BOOL),
    YF("max_concurrent", max_concurrent, YF_INT),
    YF_P("input_cost_per_1k", input_cost_per_k, has_input_price),
    YF_P("output_cost_per_1k", output_cost_per_k, has_output_price),
};

/* vision 等布尔字段的宽松真值：true/1/yes（大小写不敏感）。 */
static int svc_yaml_truthy(const char *v)
{
    return strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0;
}

/* A7-4（0.1.20）声明面：models[] 行的已知键全集 = 字段表 13 键 + 路由与
 * 身份 8 键 + capabilities。行内出现集合外键即拒载（fail-closed：错拼的
 * 声明宁可丢行，不可静默失明）。providers 段不加此约束，存量声明不受影响。 */
static int svc_yaml_key_known(const char *key)
{
    static const char *const extra[] = {"name",     "model_id",  "provider",
                                        "api_key_env", "endpoint",  "base_url",
                                        "provision",   "artifact",  "capabilities"};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
        if (strcmp(key, extra[i]) == 0) return 1;
    for (size_t i = 0; i < sizeof(svc_yaml_fields) / sizeof(svc_yaml_fields[0]); ++i)
        if (strcmp(key, svc_yaml_fields[i].key) == 0) return 1;
    return 0;
}

/* 返回行内首个契约外键名，结构合法返回 NULL。丢行必须给出可定位原因：
 * 只报聚合的 "no models found" 会让错拼的键静默失明（0.1.20 r42）。 */
static const char *svc_yaml_badkey(struct yaml_node *item)
{
    if (item->type != YAML_NODE_MAPPING)
        return "(not-a-mapping)";
    for (struct yaml_mapping_entry *e = item->mapping; e && e->key; ++e)
        if (!svc_yaml_key_known(e->key))
            return e->key;
    return NULL;
}

/* capabilities 子映射的 8 个裸名 → local_caps_t 位。 */
#define CAPB(k, f) { k, offsetof(local_caps_t, f) }
static const struct {
    const char *key;
    unsigned off;
} svc_caps_bits[] = {
    CAPB("seed", accepts_seed),
    CAPB("grammar", accepts_grammar),
    CAPB("num_ctx", accepts_num_ctx),
    CAPB("response_format", accepts_response_format),
    CAPB("cache_prompt", accepts_cache_prompt),
    CAPB("slot_id", accepts_slot_id),
    CAPB("enable_thinking", accepts_enable_thinking),
    CAPB("thinking_budget", accepts_thinking_budget),
};

/* 声明三键落盘：provision 值域 managed|external、artifact 标量路径、
 * capabilities 子映射（裸名外键 / 非映射 / 非标量值即非法）。返回 -1 拒载。 */
static int svc_yaml_decl_apply(model_entry_t *m, struct yaml_node *item)
{
    const char *pv = svc_yaml_item_str(item, "provision");
    if (pv && pv[0]) {
        if (strcmp(pv, "managed") != 0 && strcmp(pv, "external") != 0)
            return -1;
        AIRY_STRNCPY_TERM(m->provision, pv, sizeof(m->provision));
    }
    const char *art = svc_yaml_item_str(item, "artifact");
    if (art && art[0])
        AIRY_STRNCPY_TERM(m->artifact, art, sizeof(m->artifact));

    struct yaml_node *caps = yaml_get(item, "capabilities");
    if (!caps)
        return 0;
    if (caps->type != YAML_NODE_MAPPING)
        return -1;
    for (struct yaml_mapping_entry *e = caps->mapping; e && e->key; ++e) {
        size_t b = 0;
        for (; b < sizeof(svc_caps_bits) / sizeof(svc_caps_bits[0]); ++b)
            if (strcmp(e->key, svc_caps_bits[b].key) == 0)
                break;
        if (b == sizeof(svc_caps_bits) / sizeof(svc_caps_bits[0]))
            return -1;
        const char *v = yaml_as_string(e->value, NULL);
        if (!v)
            return -1;
        *(bool *)((char *)&m->caps_override + svc_caps_bits[b].off) = (bool)svc_yaml_truthy(v);
    }
    m->has_caps = 1;
    return 0;
}

/* 按表把 item 中的标量逐字段落盘；键缺失即保持 memset 后的默认值。 */
static void svc_yaml_apply(model_entry_t *m, struct yaml_node *item)
{
    for (size_t i = 0; i < sizeof(svc_yaml_fields) / sizeof(svc_yaml_fields[0]); ++i) {
        const svc_yaml_field_t *f = &svc_yaml_fields[i];
        const char *v = svc_yaml_item_str(item, f->key);
        char *base = (char *)m;
        if (!v) continue;
        if (f->kind == YF_STR)
            AIRY_STRNCPY_TERM(base + f->off, v, f->cap);
        else if (f->kind == YF_INT)
            *(int *)(base + f->off) = (int)strtol(v, NULL, 10);
        else if (f->kind == YF_TOKENS)
            *(int *)(base + f->off) = svc_tokens_parse(v);
        else if (f->kind == YF_PRICE) {
            *(double *)(base + f->off) = atof(v);
            *(int *)(base + f->flag) = 1;
        } else if (f->kind == YF_BOOL)
            *(int *)(base + f->off) = svc_yaml_truthy(v);
    }
}

/* api_key_env 与 endpoint：显式声明优先；key 缺省按行序自动映射
 * MODEL_<序号>_API_KEY（q7，local 模式无 key），endpoint 回退 base_url。 */
static void svc_yaml_route(model_entry_t *m, struct yaml_node *item, size_t row)
{
    const char *e = svc_yaml_item_str(item, "api_key_env");
    const char *mode = svc_yaml_item_str(item, "mode");
    const char *ep = svc_yaml_item_str(item, "endpoint");
    const char *bu = svc_yaml_item_str(item, "base_url");
    if (e && e[0]) {
        AIRY_STRNCPY_TERM(m->api_key_env, e, sizeof(m->api_key_env));
    } else if (!(mode && strcasecmp(mode, "local") == 0)) {
        char auto_env[64];
        snprintf(auto_env, sizeof(auto_env), "MODEL_%zu_API_KEY", row + 1);
        AIRY_STRNCPY_TERM(m->api_key_env, auto_env, sizeof(m->api_key_env));
    }
    if (ep)
        AIRY_STRNCPY_TERM(m->endpoint, ep, sizeof(m->endpoint));
    else if (bu && bu[0])
        AIRY_STRNCPY_TERM(m->endpoint, bu, sizeof(m->endpoint));
}

/* models[] 的一行 → st->models[64]。v2 表格格式：name=展示名 /
 * model_id=模型名；provider 缺省退回 name。模型名与 provider 缺一丢行；
 * 未知键 / provision 值域外 / capabilities 结构非法同样丢行（A7-4）。 */
static void svc_yaml_parse_model(svc_yaml_state_t *st, struct yaml_node *item)
{
    if (st->model_count >= 64)
        return;
    const char *n = svc_yaml_item_str(item, "name");
    const char *mid = svc_yaml_item_str(item, "model_id");
    const char *p = svc_yaml_item_str(item, "provider");
    const char *mn = (mid && mid[0]) ? mid : n;
    const char *pn = p ? p : n;
    if (!(mn && mn[0] && pn && pn[0])) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN row dropped: missing "
                     "name/model_id, STACK: svc_yaml_parse_model");
        return;
    }
    const char *bad = svc_yaml_badkey(item);
    if (bad) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN row dropped: unknown key "
                     "'%s' (name=%s), STACK: svc_yaml_parse_model", bad, mn);
        return;
    }
    model_entry_t *m = &st->models[st->model_count];
    __builtin_memset(m, 0, sizeof(*m));
    AIRY_STRNCPY_TERM(m->name, mn, sizeof(m->name));
    AIRY_STRNCPY_TERM(m->provider, pn, sizeof(m->provider));
    if (svc_yaml_decl_apply(m, item) != 0) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN row dropped: illegal "
                     "provision/artifact/capabilities (name=%s), STACK: "
                     "svc_yaml_parse_model", mn);
        return;
    }
    svc_yaml_route(m, item, st->model_count);
    svc_yaml_apply(m, item);
    st->model_count++;
}

int svc_yaml_load_state(const char *config_path, svc_yaml_state_t *st)
{
    yaml_document_t *doc = yaml_create();
    if (!doc)
        return AIRY_EINVAL;
    if (yaml_parse_file(doc, config_path) != 0) {
        yaml_destroy(doc);
        return AIRY_EINVAL;
    }

    struct yaml_node *root = yaml_root(doc);

    (void)svc_model_llm_fill(yaml_get(root, "llm"), &st->llm); /* 顺带 fill（B'） */

    struct yaml_node *models = yaml_get(root, "models");
    size_t mcount = yaml_size(models);
    for (size_t i = 0; i < mcount; ++i)
        svc_yaml_parse_model(st, yaml_get_index(models, i));

    struct yaml_node *provs = yaml_get(root, "providers");
    size_t pcount = yaml_size(provs);
    for (size_t i = 0; i < pcount; ++i)
        svc_yaml_parse_prov(st, yaml_get_index(provs, i));

    yaml_destroy(doc);
    return AIRY_OK;
}

/* Expand the simplified top-level llm: section: when it exists it takes
 * precedence over the full providers/models schema (see the comment in the
 * caller about the llm-wins precedence rule). Filled by svc_yaml_load_state. */
void svc_yaml_expand_llm(svc_yaml_state_t *st)
{
    const svc_model_llm_config_t *llm_cfg = &st->llm;
    if (!llm_cfg->model[0])
        return;

    for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
        for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
            AIRY_FREE(st->pcfg[pi].model_names[k]);
        st->pcfg[pi].model_count = 0;
    }
    st->pcfg_count = 0;

    const char *fmt = llm_cfg->api_format[0] ? llm_cfg->api_format : "chat";
    model_entry_t *m = &st->models[0];
    __builtin_memset(m, 0, sizeof(*m));
    AIRY_STRNCPY_TERM(m->name, llm_cfg->model, sizeof(m->name));
    AIRY_STRNCPY_TERM(m->provider, fmt, sizeof(m->provider));
    AIRY_STRNCPY_TERM(m->api_format, fmt, sizeof(m->api_format));
    m->max_output_tokens = llm_cfg->max_output_tokens;
    if (llm_cfg->api_key_env[0])
        AIRY_STRNCPY_TERM(m->api_key_env, llm_cfg->api_key_env, sizeof(m->api_key_env));
    if (llm_cfg->base_url[0])
        AIRY_STRNCPY_TERM(m->endpoint, llm_cfg->base_url, sizeof(m->endpoint));
    if (llm_cfg->extra_headers[0])
        AIRY_STRNCPY_TERM(m->extra_headers, llm_cfg->extra_headers, sizeof(m->extra_headers));
    st->model_count = 1;
    SVC_LOG_INFO("C-L02: SVC: expanded simplified llm section "
                 "(format=%s base_url=%s model=%s)",
                 fmt, llm_cfg->base_url[0] ? llm_cfg->base_url : "(unset)", llm_cfg->model);
}
