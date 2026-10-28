/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file types.h
 * @brief llm_d config 域 YAML 解析结果类型。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入：模型表项、
 * provider 聚合与解析结果状态。YAML 装载统一经 commons
 * utils/config_unified/yaml_minimal 完成（声明面只保留一个装载器，B13），
 * 本头不再暴露任何解析器类型。仅 service_config_yaml*.c 与
 * service_config.c 消费；跨域禁止 include 本头。
 */

#ifndef AIRY_RT_LLM_CONFIG_TYPES_H
#define AIRY_RT_LLM_CONFIG_TYPES_H

#include <stddef.h>

#include "providers/core/provider_config.h"
#include "svc_model_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

/* YAML 配置解析结果（service_config_yaml*.c 拆分）：模型条目、提供商聚合与共享
 * 解析状态；节点树由 commons 的 yaml_minimal 提供，解析时逐节点消费。 */

typedef struct {
    char name[128];
    char provider[64];
    char api_key_env[128];
    char endpoint[512];
    int timeout_sec;
    int max_retries;
    /* 模型单价（model.yaml models[].input/output_cost_per_1k），用于生成
     * cost_tracker 的 pricing rules。 */
    double input_cost_per_k;
    double output_cost_per_k;
    /* 价格是否显式声明：区分"免费模型显式 0"与"字段缺失落默认价"。 */
    int has_input_price;
    int has_output_price;
    /* v2 表格格式扩展字段（2026-08-26）：
     * mode / api_format / context_window / max_output / tool_rounds /
     * vision / thinking 来自模型连接表的每行配置。 */
    char mode[8];
    char api_format[16];
    char context_window[16];
    /* D2-9④（0.1.20）：该模型的调用方并发预算（model.yaml max_concurrent，
     * 0 = 未配置）。max_inflight = min(first_nonzero(本值,1), server_parallel)，
     * server_parallel = first_nonzero(/props.total_slots, 本值, 1)。 */
    int max_concurrent;
    /* max_output 的 token 数（由 svc_tokens_parse 在解析点一次性解释）。 */
    int max_output_tokens;
    int tool_rounds;
    int vision;
    char thinking[8];
    /* 端点附加头（model.yaml extra_headers）：线格式必需但非厂商无关的头，
     * 如 Messages 的协议版本，多条以 ';' 或换行分隔。 */
    char extra_headers[192];
    /* A7-4（0.1.20）声明面三键：托管归属（managed|external，值域外拒载）、
     * 托管工件路径（gguf）、请求体能力位显式声明（8 裸名子映射，唯一来源）。 */
    char provision[8];
    char artifact[256];
    local_caps_t caps_override;
    int has_caps;
} model_entry_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    size_t model_count;
    char api_format[16];
    char extra_headers[256];
    /* 与 model_names 同下标对齐：每模型调用方并发预算（0 = 未配置）。 */
    int model_max_concurrent[64];
} prov_cfg_t;

typedef struct {
    char name[64];
    char api_key_env[128];
    char base_url[512];
    int timeout_sec;
    int max_retries;
    char *model_names[64];
    /* 与 model_names 同下标对齐：该模型的输出上限（model.yaml max_output
     * 解析后的 token 数，0 = 未配置）。聚合与导出都必须携带，否则配置
     * 了上限也截断不了。 */
    int model_max_output[64];
    /* 与 model_names 同下标对齐：每模型调用方并发预算（D2-9④，0 = 未配置）。 */
    int model_max_concurrent[64];
    size_t model_count;
    /* 聚合出的连接声明：api_format 为适配器查找键（chat | messages |
     * local），extra_headers 为首个显式声明该字段的模型带来的端点附加头。 */
    char api_format[16];
    char extra_headers[256];
    /* A7-4：能力位取首个显式声明的模型行（与 api_format 聚合惯例同构）；
     * has_caps=0 即全 false，不派生、不猜测。 */
    local_caps_t caps_override;
    int has_caps;
} provider_agg_t;

/* YAML 装载结果：models[] 行解析结果 + providers 段声明。由
 * service_config_yaml_models.c 的 node 树 walk 填充，providers / pricing
 * 两件消费。 */
typedef struct {
    prov_cfg_t cur_p; /* providers 段当前项的构造暂存 */
    prov_cfg_t pcfg[16];
    size_t pcfg_count;
    model_entry_t models[64];
    size_t model_count;
    /* 简化 llm 段：load_state 顺带 fill，expand_llm 消费（B'：免二次解析） */
    svc_model_llm_config_t llm;
} svc_yaml_state_t;

/* service_config_yaml_models.c */
/* 装载 YAML 并装配解析状态：AIRY_OK 成功；AIRY_EINVAL 表示文件不可读/
 * 超限/内存不足（调用方按"非错误"处理，与原 fopen / parser-init 失败
 * 分支语义一致）。 */
int svc_yaml_load_state(const char *config_path, svc_yaml_state_t *st);
void svc_yaml_expand_llm(svc_yaml_state_t *st);

/* service_config_yaml_providers.c */
int svc_load_model_config_yaml(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_CONFIG_TYPES_H */
