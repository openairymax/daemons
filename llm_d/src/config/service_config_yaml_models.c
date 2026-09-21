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
#include "service.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"
#include "yaml_minimal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/types.h"

#ifdef HAVE_YAML

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

    if (st->pcfg_count < 16) {
        st->pcfg[st->pcfg_count++] = st->cur_p;
    } else {
        for (size_t k = 0; k < st->cur_p.model_count; ++k)
            AIRY_FREE(st->cur_p.model_names[k]);
        st->cur_p.model_count = 0;
    }
}

/* models[] 的一行 → st->models[64]。 */
static void svc_yaml_parse_model(svc_yaml_state_t *st, struct yaml_node *item)
{
    if (st->model_count >= 64)
        return;

    const char *n = svc_yaml_item_str(item, "name");
    const char *p = svc_yaml_item_str(item, "provider");
    const char *e = svc_yaml_item_str(item, "api_key_env");
    const char *ep = svc_yaml_item_str(item, "endpoint");
    const char *t = svc_yaml_item_str(item, "timeout_sec");
    const char *r = svc_yaml_item_str(item, "max_retries");
    const char *ic = svc_yaml_item_str(item, "input_cost_per_1k");
    const char *oc = svc_yaml_item_str(item, "output_cost_per_1k");
    /* v2 表格格式（2026-08-26）：连接表每行 name=展示名 / model_id=模型名。
     * 模型名优先取 model_id（缺省退回 name）；provider 用展示名（缺省
     * 退回 name），作为 provider 键参与聚合与路由。 */
    const char *mid = svc_yaml_item_str(item, "model_id");
    const char *mode = svc_yaml_item_str(item, "mode");
    const char *fmt = svc_yaml_item_str(item, "api_format");
    const char *bu = svc_yaml_item_str(item, "base_url");
    const char *cw = svc_yaml_item_str(item, "context_window");
    const char *mo = svc_yaml_item_str(item, "max_output");
    const char *tr = svc_yaml_item_str(item, "tool_rounds");
    const char *vi = svc_yaml_item_str(item, "vision");
    const char *th = svc_yaml_item_str(item, "thinking");

    const char *model_name = (mid && mid[0]) ? mid : (n ? n : NULL);
    const char *prov_name = p ? p : (n ? n : NULL);
    if (!(model_name && model_name[0] && prov_name && prov_name[0]))
        return;

    model_entry_t *m = &st->models[st->model_count];
    __builtin_memset(m, 0, sizeof(*m));
    AIRY_STRNCPY_TERM(m->name, model_name, sizeof(m->name));
    AIRY_STRNCPY_TERM(m->provider, prov_name, sizeof(m->provider));
    if (e && e[0]) {
        AIRY_STRNCPY_TERM(m->api_key_env, e, sizeof(m->api_key_env));
    } else if (!(mode && strcasecmp(mode, "local") == 0)) {
        /* api_key_env 自动映射（q7）：连接表行留空/缺省时按行号生成
         * MODEL_<序号>_API_KEY（序号 = 表中行序，1 起）。用户接入新
         * 模型只需在 secrets.env 增加 MODEL_N_API_KEY=xxx 一个 Key 位，
         * 无需理解 env 变量名与表格行的映射关系。local 模式无 key，
         * 保持空。 */
        char auto_env[64];
        snprintf(auto_env, sizeof(auto_env), "MODEL_%zu_API_KEY", st->model_count + 1);
        AIRY_STRNCPY_TERM(m->api_key_env, auto_env, sizeof(m->api_key_env));
    }
    if (ep) {
        AIRY_STRNCPY_TERM(m->endpoint, ep, sizeof(m->endpoint));
    } else if (bu && bu[0]) {
        /* v2：base_url 为服务根地址，按 api_format 补后缀 */
        const char *adapter = "openai";
        if (fmt && strcasecmp(fmt, "anthropic") == 0)
            adapter = "anthropic";
        snprintf(m->endpoint, sizeof(m->endpoint), "%s%s", bu,
                 (strcmp(adapter, "anthropic") == 0) ? "/messages" : "/chat/completions");
    }
    if (t)
        m->timeout_sec = (int)strtol(t, NULL, 10);
    if (r)
        m->max_retries = (int)strtol(r, NULL, 10);
    if (ic) {
        m->input_cost_per_k = atof(ic);
        m->has_input_price = 1;
    }
    if (oc) {
        m->output_cost_per_k = atof(oc);
        m->has_output_price = 1;
    }
    if (mode)
        AIRY_STRNCPY_TERM(m->mode, mode, sizeof(m->mode));
    if (fmt)
        AIRY_STRNCPY_TERM(m->api_format, fmt, sizeof(m->api_format));
    if (cw)
        AIRY_STRNCPY_TERM(m->context_window, cw, sizeof(m->context_window));
    if (mo)
        m->max_output_tokens = svc_tokens_parse(mo);
    if (tr)
        m->tool_rounds = (int)strtol(tr, NULL, 10);
    if (vi) {
        if (strcasecmp(vi, "true") == 0 || strcmp(vi, "1") == 0 ||
            strcasecmp(vi, "yes") == 0)
            m->vision = 1;
    }
    if (th)
        AIRY_STRNCPY_TERM(m->thinking, th, sizeof(m->thinking));
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
 * caller about the llm-wins precedence rule). */
void svc_yaml_expand_llm(svc_yaml_state_t *st, const char *config_path)
{
    svc_model_llm_config_t llm_cfg;
    __builtin_memset(&llm_cfg, 0, sizeof(llm_cfg));
    if (svc_model_defaults_llm_from_yaml(config_path, &llm_cfg) == 0 && llm_cfg.model[0]) {
        for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
            for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            st->pcfg[pi].model_count = 0;
        }
        st->pcfg_count = 0;
        const char *adapter = "openai";
        if (strcasecmp(llm_cfg.api_format, "anthropic") == 0)
            adapter = "anthropic";
        __builtin_memset(&st->models[0], 0, sizeof(st->models[0]));
        st->models[0].max_output_tokens = llm_cfg.max_output_tokens;
        AIRY_STRNCPY_TERM(st->models[0].name, llm_cfg.model, sizeof(st->models[0].name));
        AIRY_STRNCPY_TERM(st->models[0].provider, adapter, sizeof(st->models[0].provider));
        if (llm_cfg.api_key_env[0])
            AIRY_STRNCPY_TERM(st->models[0].api_key_env, llm_cfg.api_key_env,
                              sizeof(st->models[0].api_key_env));
        if (llm_cfg.base_url[0]) {
            if (strcmp(adapter, "anthropic") == 0)
                snprintf(st->models[0].endpoint, sizeof(st->models[0].endpoint), "%s/messages",
                         llm_cfg.base_url);
            else
                snprintf(st->models[0].endpoint, sizeof(st->models[0].endpoint),
                         "%s/chat/completions", llm_cfg.base_url);
        }
        st->model_count = 1;
        SVC_LOG_INFO("C-L02: SVC: expanded simplified llm section "
                     "(format=%s base_url=%s model=%s)",
                     llm_cfg.api_format[0] ? llm_cfg.api_format : "openai",
                     llm_cfg.base_url[0] ? llm_cfg.base_url : "(default)", llm_cfg.model);
    }
}

#endif /* HAVE_YAML */
