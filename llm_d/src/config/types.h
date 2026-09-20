/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file types.h
 * @brief llm_d config 域 YAML 解析状态机类型（HAVE_YAML 构建）。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入：扁平键值
 * 映射、模型表项、provider 聚合与全局解析状态。仅 service_config_yaml*.c
 * 与 service_config.c 消费；跨域禁止 include 本头。非 YAML 构建下本头
 * 为空翻译单元，无条件包含无害。
 */

#ifndef AIRY_RT_LLM_CONFIG_TYPES_H
#define AIRY_RT_LLM_CONFIG_TYPES_H

#include <stddef.h>

#ifdef HAVE_YAML

#include <yaml.h>

#include "providers/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- YAML config-parsing infrastructure (service_config_yaml*.c split,
 *      2026-08-27): flat key/value map + parse state shared by the global-
 *      section loader, the models state machine, provider aggregation and
 *      pricing-rule extraction. ---- */

typedef struct {
    char key[128];
    char value[512];
} yaml_kv_t;

typedef struct {
    yaml_kv_t *pairs;
    size_t count;
    size_t capacity;
} yaml_map_t;

void yaml_map_init(yaml_map_t *m);
void yaml_map_add(yaml_map_t *m, const char *key, const char *value);
const char *yaml_map_get(const yaml_map_t *m, const char *key);
void yaml_map_free(yaml_map_t *m);

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
    /* 价格字段是否在 YAML 中显式声明（区分"免费模型：显式 0"与
     * "未配置价格：字段缺失"）。免费模型（llama 等）显式 0.0 必须保留
     * 为零价规则，未配置的模型落默认价。 */
    int has_input_price;
    int has_output_price;
    /* v2 表格格式扩展字段（2026-08-26）：
     * mode / api_format / context_window / max_output / tool_rounds /
     * vision / thinking 来自模型连接表的每行配置。 */
    char mode[8];
    char api_format[16];
    char context_window[16];
    /* max_output 的 token 数（由 svc_tokens_parse 在解析点一次性解释）。 */
    int max_output_tokens;
    int tool_rounds;
    int vision;
    char thinking[8];
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
    /* 与 model_names 同下标对齐：该模型的输出上限（model.yaml max_output
     * 解析后的 token 数，0 = 未配置）。此前该字段止步于 model_entry_t，
     * 在聚合/导出两处被丢弃，导致"配置了也不生效"。 */
    int model_max_output[64];
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

/* service_config_yaml_models.c */
void svc_yaml_event_loop(yaml_parser_t *parser, svc_yaml_state_t *st, int *done);
void svc_yaml_expand_llm(svc_yaml_state_t *st, const char *config_path);

/* service_config_yaml.c / service_config_yaml_providers.c */
int svc_config_load_yaml(const char *config_path, service_config_t *cfg);
int svc_load_model_config_yaml(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* HAVE_YAML */

#endif /* AIRY_RT_LLM_CONFIG_TYPES_H */
