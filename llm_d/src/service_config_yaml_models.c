// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml_models.c
 * @brief LLM model.yaml models-section parsing (split from service_config.c,
 *        2026-08-27): libyaml event state machine over the models/providers
 *        arrays and the simplified top-level llm-section expansion.
 *
 * 解析出的 models[] 与 providers 段 kv 对存入共享的 svc_yaml_state_t，
 * 由 service_config_yaml_providers.c 聚合导出、pricing 件读取价格字段；
 * 状态结构经 llm_service_internal.h 的 HAVE_YAML 节共享。
 */

#include "airy_memory.h"
#include "service.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_service_internal.h"

#ifdef HAVE_YAML

static void svc_yaml_handle_mapping_start(svc_yaml_state_t *st)
{
    st->map_depth++;
    if (st->in_models || st->in_providers) {
        st->item_depth++;
        if (st->item_depth == 1) {
            st->nested = 0;
            st->has_pending_key = 0;
            if (st->in_providers) {
                yaml_map_free(&st->prov_map);
                yaml_map_init(&st->prov_map);
                __builtin_memset(&st->cur_p, 0, sizeof(st->cur_p));
            } else {
                yaml_map_free(&st->item_map);
                yaml_map_init(&st->item_map);
            }
        } else {
            st->nested++;
            st->has_pending_key = 0;
        }
    }
}

static void svc_yaml_finalize_provider(svc_yaml_state_t *st)
{
    const char *pn = yaml_map_get(&st->prov_map, "name");
    if (pn && pn[0]) {
        AIRY_STRNCPY_TERM(st->cur_p.name, pn, sizeof(st->cur_p.name));
        const char *pke = yaml_map_get(&st->prov_map, "api_key_env");
        if (pke)
            AIRY_STRNCPY_TERM(st->cur_p.api_key_env, pke, sizeof(st->cur_p.api_key_env));
        const char *pb = yaml_map_get(&st->prov_map, "base_url");
        if (pb)
            AIRY_STRNCPY_TERM(st->cur_p.base_url, pb, sizeof(st->cur_p.base_url));
        const char *pt = yaml_map_get(&st->prov_map, "timeout_sec");
        if (pt)
            st->cur_p.timeout_sec = (int)strtol(pt, NULL, 10);
        const char *pr = yaml_map_get(&st->prov_map, "max_retries");
        if (pr)
            st->cur_p.max_retries = (int)strtol(pr, NULL, 10);
        if (st->pcfg_count < 16) {
            st->pcfg[st->pcfg_count++] = st->cur_p;
        } else {
            for (size_t k = 0; k < st->cur_p.model_count; ++k)
                AIRY_FREE(st->cur_p.model_names[k]);
        }
    } else {
        for (size_t k = 0; k < st->cur_p.model_count; ++k)
            AIRY_FREE(st->cur_p.model_names[k]);
    }
}

static void svc_yaml_finalize_model(svc_yaml_state_t *st)
{
    if (st->model_count >= 64)
        return;
    const char *n = yaml_map_get(&st->item_map, "name");
    const char *p = yaml_map_get(&st->item_map, "provider");
    const char *e = yaml_map_get(&st->item_map, "api_key_env");
    const char *ep = yaml_map_get(&st->item_map, "endpoint");
    const char *t = yaml_map_get(&st->item_map, "timeout_sec");
    const char *r = yaml_map_get(&st->item_map, "max_retries");
    const char *ic = yaml_map_get(&st->item_map, "input_cost_per_1k");
    const char *oc = yaml_map_get(&st->item_map, "output_cost_per_1k");
    /* v2 表格格式（2026-08-26）：连接表每行 name=展示名 / model_id=模型名。
     * 模型名优先取 model_id（缺省退回 name）；provider 用展示名（缺省
     * 退回 name），作为 provider 键参与聚合与路由。 */
    const char *mid = yaml_map_get(&st->item_map, "model_id");
    const char *mode = yaml_map_get(&st->item_map, "mode");
    const char *fmt = yaml_map_get(&st->item_map, "api_format");
    const char *bu = yaml_map_get(&st->item_map, "base_url");
    const char *cw = yaml_map_get(&st->item_map, "context_window");
    const char *mo = yaml_map_get(&st->item_map, "max_output");
    const char *tr = yaml_map_get(&st->item_map, "tool_rounds");
    const char *vi = yaml_map_get(&st->item_map, "vision");
    const char *th = yaml_map_get(&st->item_map, "thinking");

    const char *model_name = (mid && mid[0]) ? mid : (n ? n : NULL);
    const char *prov_name = p ? p : (n ? n : NULL);
    if (model_name && model_name[0] && prov_name && prov_name[0]) {
        __builtin_memset(&st->models[st->model_count], 0, sizeof(model_entry_t));
        AIRY_STRNCPY_TERM(st->models[st->model_count].name, model_name,
                          sizeof(st->models[st->model_count].name));
        AIRY_STRNCPY_TERM(st->models[st->model_count].provider, prov_name,
                          sizeof(st->models[st->model_count].provider));
        if (e && e[0]) {
            AIRY_STRNCPY_TERM(st->models[st->model_count].api_key_env, e,
                              sizeof(st->models[st->model_count].api_key_env));
        } else if (!(mode && strcasecmp(mode, "local") == 0)) {
            /* api_key_env 自动映射（q7）：连接表行留空/缺省时按行号生成
             * MODEL_<序号>_API_KEY（序号 = 表中行序，1 起）。用户接入新
             * 模型只需在 secrets.env 增加 MODEL_N_API_KEY=xxx 一个 Key 位，
             * 无需理解 env 变量名与表格行的映射关系。local 模式无 key，
             * 保持空。 */
            char auto_env[64];
            snprintf(auto_env, sizeof(auto_env), "MODEL_%zu_API_KEY", st->model_count + 1);
            AIRY_STRNCPY_TERM(st->models[st->model_count].api_key_env, auto_env,
                              sizeof(st->models[st->model_count].api_key_env));
        }
        if (ep)
            AIRY_STRNCPY_TERM(st->models[st->model_count].endpoint, ep,
                              sizeof(st->models[st->model_count].endpoint));
        else if (bu && bu[0]) {
            /* v2：base_url 为服务根地址，按 api_format 补后缀 */
            const char *adapter = "openai";
            if (fmt && strcasecmp(fmt, "anthropic") == 0)
                adapter = "anthropic";
            snprintf(st->models[st->model_count].endpoint,
                     sizeof(st->models[st->model_count].endpoint), "%s%s", bu,
                     (strcmp(adapter, "anthropic") == 0) ? "/messages" : "/chat/completions");
        }
        if (t)
            st->models[st->model_count].timeout_sec = (int)strtol(t, NULL, 10);
        if (r)
            st->models[st->model_count].max_retries = (int)strtol(r, NULL, 10);
        if (ic) {
            st->models[st->model_count].input_cost_per_k = atof(ic);
            st->models[st->model_count].has_input_price = 1;
        }
        if (oc) {
            st->models[st->model_count].output_cost_per_k = atof(oc);
            st->models[st->model_count].has_output_price = 1;
        }
        if (mode)
            AIRY_STRNCPY_TERM(st->models[st->model_count].mode, mode,
                              sizeof(st->models[st->model_count].mode));
        if (fmt)
            AIRY_STRNCPY_TERM(st->models[st->model_count].api_format, fmt,
                              sizeof(st->models[st->model_count].api_format));
        if (cw)
            AIRY_STRNCPY_TERM(st->models[st->model_count].context_window, cw,
                              sizeof(st->models[st->model_count].context_window));
        if (mo)
            AIRY_STRNCPY_TERM(st->models[st->model_count].max_output, mo,
                              sizeof(st->models[st->model_count].max_output));
        if (tr)
            st->models[st->model_count].tool_rounds = (int)strtol(tr, NULL, 10);
        if (vi) {
            if (strcasecmp(vi, "true") == 0 || strcmp(vi, "1") == 0 ||
                strcasecmp(vi, "yes") == 0)
                st->models[st->model_count].vision = 1;
        }
        if (th)
            AIRY_STRNCPY_TERM(st->models[st->model_count].thinking, th,
                              sizeof(st->models[st->model_count].thinking));
        st->model_count++;
    }
}

static void svc_yaml_handle_mapping_end(svc_yaml_state_t *st)
{
    if (st->in_models || st->in_providers) {
        if (st->item_depth == 1 && st->nested == 0) {
            if (st->in_providers)
                svc_yaml_finalize_provider(st);
            else
                svc_yaml_finalize_model(st);
        }
        st->item_depth--;
        if (st->item_depth == 0) {
            st->nested = 0;
        } else if (st->nested > 0) {
            st->nested--;
        }
    }
    st->map_depth--;
}

static void svc_yaml_handle_sequence_end(svc_yaml_state_t *st)
{
    st->seq_depth--;
    if ((st->in_models || st->in_providers) && st->item_depth >= 1 && st->nested > 0)
        st->nested--;
    if (st->in_models && st->item_depth == 0 && st->seq_depth <= 1)
        st->in_models = 0;
    if (st->in_providers && st->item_depth == 0 && st->seq_depth <= 1)
        st->in_providers = 0;
    if (st->in_providers && st->item_depth == 1 && st->has_pending_key &&
        strcmp(st->pending_key, "models") == 0)
        st->has_pending_key = 0;
}

static void svc_yaml_handle_scalar(svc_yaml_state_t *st, const char *val)
{
    if (!st->in_models && !st->in_providers && st->map_depth == 1 && val) {
        if (strcmp(val, "models") == 0) {
            st->in_models = 1;
            st->has_pending_key = 0;
        } else if (strcmp(val, "providers") == 0) {
            st->in_providers = 1;
            st->has_pending_key = 0;
        }
    } else if ((st->in_models || st->in_providers) && st->item_depth == 1 &&
               st->nested == 0 && val) {
        if (!st->has_pending_key) {
            AIRY_STRNCPY_TERM(st->pending_key, val, sizeof(st->pending_key));
            st->has_pending_key = 1;
        } else {
            if (st->in_models)
                yaml_map_add(&st->item_map, st->pending_key, val);
            else
                yaml_map_add(&st->prov_map, st->pending_key, val);
            st->has_pending_key = 0;
        }
    } else if (st->in_providers && st->item_depth == 1 && st->nested >= 1 && val &&
               st->has_pending_key && strcmp(st->pending_key, "models") == 0) {
        if (st->cur_p.model_count < 64)
            st->cur_p.model_names[st->cur_p.model_count++] = AIRY_STRDUP(val);
    }
}

void svc_yaml_event_loop(yaml_parser_t *parser, svc_yaml_state_t *st, int *done)
{
    yaml_event_t event;
    while (!*done) {
        if (!yaml_parser_parse(parser, &event))
            break;
        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            *done = 1;
            break;
        case YAML_MAPPING_START_EVENT:
            svc_yaml_handle_mapping_start(st);
            break;
        case YAML_MAPPING_END_EVENT:
            svc_yaml_handle_mapping_end(st);
            break;
        case YAML_SEQUENCE_START_EVENT:
            st->seq_depth++;
            if ((st->in_models || st->in_providers) && st->item_depth >= 1)
                st->nested++;
            break;
        case YAML_SEQUENCE_END_EVENT:
            svc_yaml_handle_sequence_end(st);
            break;
        case YAML_SCALAR_EVENT:
            svc_yaml_handle_scalar(st, (const char *)event.data.scalar.value);
            break;
        default:
            break;
        }
        yaml_event_delete(&event);
    }
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
