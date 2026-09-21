// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml_pricing.c
 * @brief LLM pricing-rule extraction from a YAML model config (split from
 *        service_config.c, 2026-08-27): reuse the shared yaml parse state
 *        produced by the commons yaml_minimal node-tree walk to convert
 *        models[].input/output_cost_per_1k into pricing_rule_t entries.
 *
 * 解析状态经 config/types.h 共享。
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <stdlib.h>

#include "config/internal.h"
#include "config/types.h"

#ifdef HAVE_YAML

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

    svc_yaml_state_t st;
    __builtin_memset(&st, 0, sizeof(st));
    if (svc_yaml_load_state(config_path, &st) != AIRY_OK)
        return 0;

    /* 释放 providers 段解析时 strdup 的 model_names。cur_p 是浅拷贝，
     * pcfg 数组是这些字符串的唯一持有者，只清理 pcfg 即可（cur_p 副本
     * 已由 parse_provider 的 memset 清零，不重复释放）。 */
    for (size_t pi = 0; pi < st.pcfg_count; ++pi) {
        for (size_t k = 0; k < st.pcfg[pi].model_count; ++k)
            AIRY_FREE(st.pcfg[pi].model_names[k]);
        st.pcfg[pi].model_count = 0;
    }

    /* 注意：此处不调用 svc_yaml_expand_llm —— llm 简化段只有模型名没有
     * 价格，若替换 models 会把价格全丢。价格规则覆盖完整 models 列表，
     * 名字匹配到 llm 段扩展出的模型同样能查到价（SSoT：价格只定义在
     * models[].input/output_cost_per_1k）。 */

    int n = 0;
    for (size_t i = 0; i < st.model_count; ++i) {
        /* 只注册"显式声明了价格"的模型：显式 0（免费模型，如 llama 本地
         * 推理）保留为零价规则，字段缺失（未配置价格）的模型跳过并落
         * 默认价——此前 `> 0.0` 过滤把免费模型也踢掉，导致其被默认价
         * 计费，金额失真。 */
        if (st.models[i].has_input_price || st.models[i].has_output_price)
            n++;
    }
    if (n == 0)
        return 0;

    pricing_rule_t *rules = (pricing_rule_t *)AIRY_CALLOC((size_t)n, sizeof(pricing_rule_t));
    if (!rules)
        return 0;
    int idx = 0;
    for (size_t i = 0; i < st.model_count; ++i) {
        if (!st.models[i].has_input_price && !st.models[i].has_output_price)
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
