// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config.c
 * @brief LLM service config-loading domain: JSON/YAML parsing, provider/
 *        model config loading, pricing-rule parsing and release.
 */

#include "airy_memory.h"
#include "daemon_defaults.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_YAML
#include <yaml.h>
#endif

#include "llm_service_internal.h"

#ifdef HAVE_YAML
/* P0.18.3 fix: the YAML branch is only compiled when HAVE_YAML is on, so
 * forward-declare before the later definition, otherwise
 * -Werror=implicit-function-declaration fires at svc_config_load. */
int svc_config_load_yaml(const char *config_path, service_config_t *cfg);
#endif

int ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len)
        return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/**
 * @brief Load pricing rules
 * @param root  JSON root node
 * @param count Output rule count
 * @return Rule array (caller frees), NULL on failure
 */
pricing_rule_t *load_pricing_rules(cJSON *root, int *count)
{
    if (!root || !count) {
        SVC_LOG_ERROR("load_pricing_rules: NULL parameter (root=%p, count=%p)", (const void *)root,
                      (const void *)count);
        *count = 0;
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *pricing = cJSON_GetObjectItem(root, "pricing");
    if (!pricing || !cJSON_IsArray(pricing)) {
        SVC_LOG_ERROR("load_pricing_rules: pricing array missing or not an array in config");
        *count = 0;
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    int n = cJSON_GetArraySize(pricing);
    pricing_rule_t *rules = (pricing_rule_t *)AIRY_CALLOC((size_t)n, sizeof(pricing_rule_t));
    if (!rules) {
        SVC_LOG_ERROR("load_pricing_rules: calloc failed for %d pricing rules", n);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(pricing, i);
        cJSON *pattern = cJSON_GetObjectItem(item, "pattern");
        cJSON *input = cJSON_GetObjectItem(item, "input_price_per_k");
        cJSON *output = cJSON_GetObjectItem(item, "output_price_per_k");

        if (cJSON_IsString(pattern) && cJSON_IsNumber(input) && cJSON_IsNumber(output)) {
            rules[i].model_pattern = AIRY_STRDUP(pattern->valuestring);
            if (!rules[i].model_pattern) {

                SVC_LOG_ERROR("load_pricing_rules: strdup failed for model_pattern at index %d", i);
                for (int j = 0; j < i; ++j) {
                    AIRY_FREE((void *)rules[j].model_pattern);
                }
                AIRY_FREE(rules);
                *count = 0;
                AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
            }
            rules[i].input_price_per_k = input->valuedouble;
            rules[i].output_price_per_k = output->valuedouble;
        } else {
            rules[i].model_pattern = NULL;
        }
    }

    *count = n;
    return rules;
}

void free_pricing_rules(pricing_rule_t *rules, int count)
{
    if (!rules)
        return;
    for (int i = 0; i < count; ++i) {
        AIRY_FREE((void *)rules[i].model_pattern);
    }
    AIRY_FREE(rules);
}

/**
 * @brief Load the service config
 * @param config_path Config file path
 * @param cfg         Output config
 * @return 0 on success, non-zero on failure
 */
int svc_config_load(const char *config_path, service_config_t *cfg)
{
    if (!cfg || !config_path) {
        SVC_LOG_ERROR("C-L02: SVC: CONFIG-FAIL NULL parameter, STACK: svc_config_load");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (ends_with(config_path, ".yaml") || ends_with(config_path, ".yml")) {
#ifdef HAVE_YAML
        return svc_config_load_yaml(config_path, cfg);
#else
        SVC_LOG_WARN("C-L02: SVC: CONFIG-WARN YAML not compiled, STACK: svc_config_load");
        __builtin_memset(cfg, 0, sizeof(service_config_t));
        cfg->llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
        cfg->llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
        cfg->max_retries = AIRY_DEFAULT_MAX_RETRIES;
        cfg->timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;
        return 0;
#endif
    }

    __builtin_memset(cfg, 0, sizeof(service_config_t));

    cfg->llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    cfg->llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    cfg->max_retries = AIRY_DEFAULT_MAX_RETRIES;
    cfg->timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("C-L02: SVC: CONFIG-WARN cannot open file, STACK: svc_config_load");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)AIRY_MALLOC((size_t)len + 1);
    if (!content) {
        SVC_LOG_ERROR("C-L02: SVC: CONFIG-FAIL malloc, STACK: svc_config_load");
        fclose(f);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t read_len = fread(content, 1, (size_t)len, f);
    if (read_len != (size_t)len) {
        SVC_LOG_ERROR("C-L02: SVC: CONFIG-FAIL fread, STACK: svc_config_load");
        AIRY_FREE(content);
        fclose(f);
        return AIRY_ERR_IO;
    }
    content[read_len] = '\0';
    fclose(f);

    CJSON_PARSE_GUARD(root, content, {
        AIRY_FREE(content);
        SVC_LOG_WARN("C-L02: SVC: CONFIG-WARN parse failed, STACK: svc_config_load");
        return 0;
    });
    AIRY_FREE(content);

    cJSON *item;

    item = cJSON_GetObjectItem(root, "llm_cache_capacity");
    if (item && cJSON_IsNumber(item)) {
        cfg->llm_cache_capacity = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "llm_cache_ttl_sec");
    if (item && cJSON_IsNumber(item)) {
        cfg->llm_cache_ttl_sec = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "max_retries");
    if (item && cJSON_IsNumber(item)) {
        cfg->max_retries = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "timeout_ms");
    if (item && cJSON_IsNumber(item)) {
        cfg->timeout_ms = item->valueint;
    }

    item = cJSON_GetObjectItem(root, "token_encoding");
    if (item && cJSON_IsString(item)) {
        size_t enc_len = strlen(item->valuestring);
        if (enc_len < sizeof(cfg->token_encoding)) {
            __builtin_memcpy((char *)cfg->token_encoding, item->valuestring, enc_len + 1);
        }
    }

    return AIRY_OK;
}

#ifdef HAVE_YAML

typedef struct {
    char key[128];
    char value[512];
} yaml_kv_t;

typedef struct {
    yaml_kv_t *pairs;
    size_t count;
    size_t capacity;
} yaml_map_t;

static void yaml_map_init(yaml_map_t *m)
{
    m->pairs = NULL;
    m->count = 0;
    m->capacity = 0;
}

static void yaml_map_add(yaml_map_t *m, const char *key, const char *value)
{
    if (!key || !value)
        return;
    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity == 0 ? 16 : m->capacity * 2;
        yaml_kv_t *new_pairs = (yaml_kv_t *)AIRY_REALLOC(m->pairs, new_cap * sizeof(yaml_kv_t));
        if (!new_pairs)
            return;
        m->pairs = new_pairs;
        m->capacity = new_cap;
    }
    AIRY_STRNCPY_TERM(m->pairs[m->count].key, key, sizeof(m->pairs[m->count].key));
    AIRY_STRNCPY_TERM(m->pairs[m->count].value, value, sizeof(m->pairs[m->count].value));
    m->count++;
}

static const char *yaml_map_get(const yaml_map_t *m, const char *key)
{
    if (!m || !key) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    for (size_t i = 0; i < m->count; ++i) {
        if (strcmp(m->pairs[i].key, key) == 0)
            return m->pairs[i].value;
    }
    AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
}

static void yaml_map_free(yaml_map_t *m)
{
    AIRY_FREE(m->pairs);
    m->pairs = NULL;
    m->count = 0;
    m->capacity = 0;
}

int svc_config_load_yaml(const char *config_path, service_config_t *cfg)
{
    if (!cfg || !config_path)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(cfg, 0, sizeof(service_config_t));
    cfg->llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    cfg->llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    cfg->max_retries = AIRY_DEFAULT_MAX_RETRIES;
    cfg->timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN cannot open YAML, STACK: svc_config_load_yaml");
        return 0;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN YAML parser init, STACK: svc_config_load_yaml");
        return 0;
    }
    yaml_parser_set_input_file(&parser, f);

    yaml_event_t event;
    yaml_map_t current_map;
    yaml_map_init(&current_map);
    int in_global = 0;
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            SVC_LOG_WARN(
                "C-L02: SVC: MODEL-CONFIG-WARN YAML parse error, STACK: svc_config_load_yaml");
            break;
        }
        if (event.type == YAML_STREAM_END_EVENT) {
            done = 1;
        } else if (event.type == YAML_SCALAR_EVENT) {
            const char *val = (const char *)event.data.scalar.value;
            if (val && strcmp(val, "global") == 0) {
                in_global = 1;
            } else if (in_global && val) {
                char key_buf[128];
                AIRY_STRNCPY_TERM(key_buf, val, sizeof(key_buf));

                yaml_event_t val_event;
                if (yaml_parser_parse(&parser, &val_event)) {
                    if (val_event.type == YAML_SCALAR_EVENT) {
                        const char *v = (const char *)val_event.data.scalar.value;
                        if (v)
                            yaml_map_add(&current_map, key_buf, v);
                    }
                    yaml_event_delete(&val_event);
                }
            }
        } else if (event.type == YAML_MAPPING_END_EVENT) {
            in_global = 0;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);

    const char *val;
    if ((val = yaml_map_get(&current_map, "llm_cache_capacity"))) {
        cfg->llm_cache_capacity = (size_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "llm_cache_ttl_sec"))) {
        cfg->llm_cache_ttl_sec = (uint32_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "max_retries"))) {
        cfg->max_retries = (int)strtol(val, NULL, 10);
    }
    if ((val = yaml_map_get(&current_map, "timeout_ms"))) {
        cfg->timeout_ms = (uint32_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "token_encoding"))) {
        size_t enc_len = strlen(val);
        if (enc_len < sizeof(cfg->token_encoding)) {
            __builtin_memcpy((char *)cfg->token_encoding, val, enc_len + 1);
        }
    }

    yaml_map_free(&current_map);
    SVC_LOG_INFO("C-L02: SVC: MODEL-CONFIG-OK YAML loaded, STACK: svc_config_load_yaml");
    return AIRY_OK;
}

typedef struct {
    char name[128];
    char provider[64];
    char api_key_env[128];
    char endpoint[512];
    int timeout_sec;
    int max_retries;
    /* 2.1.1.5 修复：模型单价（model.yaml models[].input/output_cost_per_1k），
     * 用于生成 cost_tracker 的 pricing rules——此前 YAML 配置完全不加载
     * 价格，计费全部落到默认价 0.001/0.002，金额不真实。 */
    double input_cost_per_k;
    double output_cost_per_k;
} model_entry_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    size_t model_count;
} prov_cfg_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    size_t model_count;
} provider_agg_t;

typedef struct {
    yaml_map_t item_map;
    yaml_map_t prov_map;
    prov_cfg_t cur_p;
    prov_cfg_t pcfg[16];
    size_t pcfg_count;
    model_entry_t models[64];
    size_t model_count;
    int map_depth;
    int seq_depth;
    int in_models;
    int in_providers;
    int item_depth;
    int nested;
    char pending_key[128];
    int has_pending_key;
} svc_yaml_state_t;

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

    if (n && p) {
        __builtin_memset(&st->models[st->model_count], 0, sizeof(model_entry_t));
        AIRY_STRNCPY_TERM(st->models[st->model_count].name, n,
                          sizeof(st->models[st->model_count].name));
        AIRY_STRNCPY_TERM(st->models[st->model_count].provider, p,
                          sizeof(st->models[st->model_count].provider));
        if (e)
            AIRY_STRNCPY_TERM(st->models[st->model_count].api_key_env, e,
                              sizeof(st->models[st->model_count].api_key_env));
        if (ep)
            AIRY_STRNCPY_TERM(st->models[st->model_count].endpoint, ep,
                              sizeof(st->models[st->model_count].endpoint));
        if (t)
            st->models[st->model_count].timeout_sec = (int)strtol(t, NULL, 10);
        if (r)
            st->models[st->model_count].max_retries = (int)strtol(r, NULL, 10);
        if (ic)
            st->models[st->model_count].input_cost_per_k = atof(ic);
        if (oc)
            st->models[st->model_count].output_cost_per_k = atof(oc);
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

static void svc_yaml_event_loop(yaml_parser_t *parser, svc_yaml_state_t *st, int *done)
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
static void svc_yaml_expand_llm(svc_yaml_state_t *st, const char *config_path)
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

/* Aggregate providers from the models list: first model of a provider seeds
 * the provider record, later models append to its model_names. */
static void svc_yaml_aggregate_providers(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t i = 0; i < st->model_count; ++i) {
        size_t j = 0;
        for (; j < *prov_count; ++j) {
            if (strcmp(provs[j].name, st->models[i].provider) == 0)
                break;
        }
        if (j == *prov_count) {
            if (*prov_count >= 16)
                break;
            __builtin_memset(&provs[*prov_count], 0, sizeof(provider_agg_t));
            AIRY_STRNCPY_TERM(provs[*prov_count].name, st->models[i].provider,
                              sizeof(provs[*prov_count].name));
            if (st->models[i].api_key_env[0])
                AIRY_STRNCPY_TERM(provs[*prov_count].api_key_env, st->models[i].api_key_env,
                                  sizeof(provs[*prov_count].api_key_env));
            (*prov_count)++;
        }
        if (provs[j].model_count < 64) {
            provs[j].model_names[provs[j].model_count++] = AIRY_STRDUP(st->models[i].name);
        }
        if (!provs[j].base_url[0] && st->models[i].endpoint[0]) {
            const char *suffix = strstr(st->models[i].endpoint, "/chat/completions");
            if (!suffix)
                suffix = strstr(st->models[i].endpoint, "/messages");
            if (suffix && suffix != st->models[i].endpoint) {
                size_t base_len = (size_t)(suffix - st->models[i].endpoint);
                if (base_len < sizeof(provs[j].base_url)) {
                    __builtin_memcpy(provs[j].base_url, st->models[i].endpoint, base_len);
                    provs[j].base_url[base_len] = '\0';
                }
            } else {
                AIRY_STRNCPY_TERM(provs[j].base_url, st->models[i].endpoint,
                                  sizeof(provs[j].base_url));
            }
        }
        if (st->models[i].timeout_sec > provs[j].timeout_sec)
            provs[j].timeout_sec = st->models[i].timeout_sec;
        if (st->models[i].max_retries > provs[j].max_retries)
            provs[j].max_retries = st->models[i].max_retries;
    }
}

/* Merge the providers-section parse results (authoritative base_url/
 * api_key_env source) into the aggregated provider records. */
static void svc_yaml_merge_provider_cfgs(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
        size_t j = 0;
        for (; j < *prov_count; ++j) {
            if (strcmp(provs[j].name, st->pcfg[pi].name) == 0)
                break;
        }
        if (j == *prov_count) {
            if (*prov_count >= 16) {
                for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
                    AIRY_FREE(st->pcfg[pi].model_names[k]);
                continue;
            }
            __builtin_memset(&provs[*prov_count], 0, sizeof(provider_agg_t));
            AIRY_STRNCPY_TERM(provs[*prov_count].name, st->pcfg[pi].name,
                              sizeof(provs[*prov_count].name));
            (*prov_count)++;
        }

        if (st->pcfg[pi].base_url[0])
            AIRY_STRNCPY_TERM(provs[j].base_url, st->pcfg[pi].base_url,
                              sizeof(provs[j].base_url));
        if (st->pcfg[pi].api_key_env[0])
            AIRY_STRNCPY_TERM(provs[j].api_key_env, st->pcfg[pi].api_key_env,
                              sizeof(provs[j].api_key_env));
        if (st->pcfg[pi].timeout_sec > provs[j].timeout_sec)
            provs[j].timeout_sec = st->pcfg[pi].timeout_sec;
        if (st->pcfg[pi].max_retries > provs[j].max_retries)
            provs[j].max_retries = st->pcfg[pi].max_retries;

        for (size_t k = 0; k < st->pcfg[pi].model_count; ++k) {
            int dup = 0;
            for (size_t m = 0; m < provs[j].model_count; ++m) {
                if (strcmp(provs[j].model_names[m], st->pcfg[pi].model_names[k]) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            } else if (provs[j].model_count < 64) {
                provs[j].model_names[provs[j].model_count++] = st->pcfg[pi].model_names[k];
            } else {
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            }
        }
    }
}

static int svc_yaml_build_result(provider_agg_t *provs, size_t prov_count,
                                 provider_config_t **out_providers, size_t *out_count)
{
    provider_config_t *result =
        (provider_config_t *)AIRY_CALLOC(prov_count + 1, sizeof(provider_config_t));
    if (!result) {
        for (size_t j = 0; j < prov_count; ++j) {
            for (size_t k = 0; k < provs[j].model_count; ++k)
                AIRY_FREE(provs[j].model_names[k]);
        }
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < prov_count; ++i) {
        result[i].name = AIRY_STRDUP(provs[i].name);
        if (provs[i].api_key_env[0]) {
            char env_prefix[8] = "env:";
            size_t env_key_len = strlen(provs[i].api_key_env);
            char *key_buf = (char *)AIRY_MALLOC(4 + env_key_len + 1);
            if (key_buf) {
                __builtin_memcpy(key_buf, env_prefix, 4);
                __builtin_memcpy(key_buf + 4, provs[i].api_key_env, env_key_len + 1);
                result[i].api_key = key_buf;
            }
        }
        if (provs[i].base_url[0])
            result[i].api_base = AIRY_STRDUP(provs[i].base_url);
        result[i].timeout_sec = (double)provs[i].timeout_sec;
        result[i].max_retries = provs[i].max_retries;
        if (provs[i].model_count > 0) {
            char **marr = (char **)AIRY_CALLOC(provs[i].model_count + 1, sizeof(char *));
            if (marr) {
                for (size_t k = 0; k < provs[i].model_count; ++k)
                    marr[k] = provs[i].model_names[k];
                marr[provs[i].model_count] = NULL;
            } else {
                for (size_t k = 0; k < provs[i].model_count; ++k)
                    AIRY_FREE(provs[i].model_names[k]);
            }
            result[i].models = marr;
        }
    }

    *out_providers = result;
    *out_count = prov_count;
    return AIRY_OK;
}

int svc_load_model_config_yaml(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count)
{
    if (!config_path || !out_providers || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_providers = NULL;
    *out_count = 0;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN cannot open model config, STACK: "
                     "svc_load_model_config_yaml");
        return 0;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN YAML parser init, STACK: svc_load_model_config_yaml");
        return 0;
    }
    yaml_parser_set_input_file(&parser, f);

    svc_yaml_state_t st;
    __builtin_memset(&st, 0, sizeof(st));
    yaml_map_init(&st.item_map);
    yaml_map_init(&st.prov_map);
    int done = 0;
    svc_yaml_event_loop(&parser, &st, &done);

    yaml_parser_delete(&parser);
    fclose(f);
    yaml_map_free(&st.item_map);
    yaml_map_free(&st.prov_map);

    /* Simplified llm section expansion: when the top-level llm: mapping
     * exists and model is non-empty, it takes precedence over the full
     * providers/models (see svc_yaml_expand_llm). */
    svc_yaml_expand_llm(&st, config_path);
    if (st.model_count == 0 && st.pcfg_count == 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN no models found, STACK: svc_load_model_config_yaml");
        return 0;
    }

    provider_agg_t provs[16];
    size_t prov_count = 0;
    svc_yaml_aggregate_providers(&st, provs, &prov_count);
    svc_yaml_merge_provider_cfgs(&st, provs, &prov_count);
    int rc = svc_yaml_build_result(provs, prov_count, out_providers, out_count);
    if (rc == AIRY_OK) {
        SVC_LOG_INFO(
            "C-L02: SVC: MODEL-CONFIG-OK YAML providers=%zu, STACK: svc_load_model_config_yaml",
            prov_count);
    }
    return rc;
}

/**
 * @brief Load pricing rules from a YAML model config (model.yaml).
 *
 * 2.1.1.5 修复：cost_tracker 的价格此前仅从 JSON 配置的 pricing 数组加载，
 * YAML 配置（当前唯一入口）完全不加载价格，全部模型落到默认价
 * 0.001/0.002，计费金额不真实。本函数复用 svc_yaml 解析状态机，将
 * models[].{name, input_cost_per_1k, output_cost_per_1k} 转换为
 * pricing_rule_t（model_pattern=模型名精确匹配）。
 *
 * @param config_path YAML 文件路径
 * @param out_rules  输出规则数组（AIRY_MALLOC，调用方 free_pricing_rules 释放）
 * @param out_count  输出规则数
 * @return 0 成功；无价格模型或文件不可读时 *out_count=0（非错误）
 */
int load_pricing_rules_from_yaml(const char *config_path, pricing_rule_t **out_rules,
                                 int *out_count)
{
    if (!config_path || !out_rules || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_rules = NULL;
    *out_count = 0;

    FILE *f = fopen(config_path, "rb");
    if (!f)
        return 0;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return 0;
    }
    yaml_parser_set_input_file(&parser, f);

    svc_yaml_state_t st;
    __builtin_memset(&st, 0, sizeof(st));
    yaml_map_init(&st.item_map);
    yaml_map_init(&st.prov_map);
    int done = 0;
    svc_yaml_event_loop(&parser, &st, &done);

    yaml_parser_delete(&parser);
    fclose(f);
    yaml_map_free(&st.item_map);
    yaml_map_free(&st.prov_map);

    /* 注意：此处不调用 svc_yaml_expand_llm —— llm 简化段只有模型名没有
     * 价格，若替换 models 会把价格全丢。价格规则覆盖完整 models 列表，
     * 名字匹配到 llm 段扩展出的模型同样能查到价（SSoT：价格只定义在
     * models[].input/output_cost_per_1k）。 */

    int n = 0;
    for (size_t i = 0; i < st.model_count; ++i) {
        if (st.models[i].input_cost_per_k > 0.0 || st.models[i].output_cost_per_k > 0.0)
            n++;
    }
    if (n == 0)
        return 0;

    pricing_rule_t *rules = (pricing_rule_t *)AIRY_CALLOC((size_t)n, sizeof(pricing_rule_t));
    if (!rules)
        return 0;
    int idx = 0;
    for (size_t i = 0; i < st.model_count; ++i) {
        if (st.models[i].input_cost_per_k <= 0.0 && st.models[i].output_cost_per_k <= 0.0)
            continue;
        rules[idx].model_pattern = AIRY_STRDUP(st.models[i].name);
        if (!rules[idx].model_pattern) {
            for (int j = 0; j < idx; ++j)
                AIRY_FREE((void *)rules[j].model_pattern);
            AIRY_FREE(rules);
            return 0;
        }
        rules[idx].input_price_per_k = st.models[i].input_cost_per_k;
        rules[idx].output_price_per_k = st.models[i].output_cost_per_k;
        idx++;
    }
    *out_rules = rules;
    *out_count = n;
    SVC_LOG_INFO("C-L02: SVC: pricing rules from YAML models=%d", n);
    return 0;
}

#endif /* HAVE_YAML */
static int svc_load_model_config_json(const char *config_path, provider_config_t **out_providers,
                                      size_t *out_count)
{
    if (!config_path || !out_providers || !out_count) {
        SVC_LOG_ERROR(
            "C-L02: SVC: MODEL-CONFIG-FAIL NULL parameter, STACK: svc_load_model_config_json");
        return AIRY_ERR_INVALID_PARAM;
    }

    *out_providers = NULL;
    *out_count = 0;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN cannot open file, STACK: svc_load_model_config_json");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)AIRY_MALLOC((size_t)len + 1);
    if (!content) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL malloc, STACK: svc_load_model_config_json");
        fclose(f);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t read_len = fread(content, 1, (size_t)len, f);
    content[read_len] = '\0';
    fclose(f);

    CJSON_PARSE_GUARD(root, content, {
        AIRY_FREE(content);
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN parse failed, STACK: svc_load_model_config_json");
        return 0;
    });
    AIRY_FREE(content);

    cJSON *providers_arr = cJSON_GetObjectItem(root, "providers");
    if (!providers_arr || !cJSON_IsArray(providers_arr)) {

        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN no providers, STACK: svc_load_model_config_json");
        return 0;
    }

    int n = cJSON_GetArraySize(providers_arr);
    if (n <= 0) {

        return 0;
    }

    provider_config_t *result =
        (provider_config_t *)AIRY_CALLOC((size_t)n + 1, sizeof(provider_config_t));
    if (!result) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL calloc, STACK: svc_load_model_config_json");

        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t valid_count = 0;
    for (int i = 0; i < n; ++i) {
        cJSON *pitem = cJSON_GetArrayItem(providers_arr, i);
        cJSON *pname = cJSON_GetObjectItem(pitem, "name");
        cJSON *pkey_env = cJSON_GetObjectItem(pitem, "api_key_env");
        cJSON *pbase = cJSON_GetObjectItem(pitem, "base_url");
        cJSON *ptimeout = cJSON_GetObjectItem(pitem, "timeout_sec");
        cJSON *pretries = cJSON_GetObjectItem(pitem, "max_retries");
        cJSON *pmodels = cJSON_GetObjectItem(pitem, "models");

        if (!cJSON_IsString(pname))
            continue;

        provider_config_t *pcfg = &result[valid_count];
        pcfg->name = AIRY_STRDUP(pname->valuestring);

        if (cJSON_IsString(pkey_env) && pkey_env->valuestring[0]) {
            size_t env_len = strlen(pkey_env->valuestring);
            char *key_buf = (char *)AIRY_MALLOC(4 + env_len + 1);
            if (key_buf) {
                __builtin_memcpy(key_buf, "env:", 4);
                __builtin_memcpy(key_buf + 4, pkey_env->valuestring, env_len + 1);
                pcfg->api_key = key_buf;
            }
        }

        if (cJSON_IsString(pbase))
            pcfg->api_base = AIRY_STRDUP(pbase->valuestring);
        if (cJSON_IsNumber(ptimeout))
            pcfg->timeout_sec = ptimeout->valuedouble;
        if (cJSON_IsNumber(pretries))
            pcfg->max_retries = pretries->valueint;

        if (cJSON_IsArray(pmodels)) {
            int mcount = cJSON_GetArraySize(pmodels);
            char **marr = (char **)AIRY_CALLOC((size_t)mcount + 1, sizeof(char *));
            if (marr) {
                for (int j = 0; j < mcount; ++j) {
                    cJSON *mitem = cJSON_GetArrayItem(pmodels, j);
                    if (cJSON_IsString(mitem))
                        marr[j] = AIRY_STRDUP(mitem->valuestring);
                }
                marr[mcount] = NULL;
                pcfg->models = marr;
            }
        }
        valid_count++;
    }

    *out_providers = result;
    *out_count = valid_count;
    SVC_LOG_INFO("C-L02: SVC: MODEL-CONFIG-OK providers=%zu, STACK: svc_load_model_config_json",
                 valid_count);
    return AIRY_OK;
}

int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count)
{
    if (!config_path || !out_providers || !out_count) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL NULL parameter, STACK: svc_load_model_config");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (ends_with(config_path, ".yaml") || ends_with(config_path, ".yml")) {
#ifdef HAVE_YAML
        return svc_load_model_config_yaml(config_path, out_providers, out_count);
#else
        SVC_LOG_ERROR(
            "C-L02: SVC: MODEL-CONFIG-FAIL YAML not compiled, STACK: svc_load_model_config");
        return AIRY_ERR_NOT_SUPPORTED;
#endif
    }
    return svc_load_model_config_json(config_path, out_providers, out_count);
}
