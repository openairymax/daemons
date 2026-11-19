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
#include "svc_logger.h"

#include <stdlib.h>

#include "config/pricing_api.h"
#include "config/types.h"

/* 只注册"显式声明了价格"的模型：显式 0（免费模型）保留为零价规则，
 * 判据是 has_*_price 而非数值大小。 */
static int pricing_has_price(const model_entry_t *m)
{
    return m->has_input_price || m->has_output_price;
}

/* 释放 providers 段解析时 strdup 的 model_names：pcfg 是这些字符串的
 * 唯一持有者，只清理 pcfg 即可。 */
static void pricing_rel_models(svc_yaml_state_t *st)
{
    for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
        for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
            AIRY_FREE(st->pcfg[pi].model_names[k]);
        st->pcfg[pi].model_count = 0;
    }
}

/* 按声明顺序把价格模型拷贝成规则表；任一项 strdup 失败即全量回滚。 */
static pricing_rule_t *pricing_fill_rules(const svc_yaml_state_t *st, int n)
{
    pricing_rule_t *rules = (pricing_rule_t *)AIRY_CALLOC((size_t)n, sizeof(pricing_rule_t));

    if (!rules)
        return NULL;
    int idx = 0;
    for (size_t i = 0; i < st->model_count; ++i) {
        if (!pricing_has_price(&st->models[i]))
            continue;
        rules[idx].model_pattern = AIRY_STRDUP(st->models[i].name);
        if (!rules[idx].model_pattern) {
            for (int j = 0; j < idx; ++j)
                AIRY_FREE((void *)rules[j].model_pattern);
            AIRY_FREE(rules);
            return NULL;
        }
        rules[idx].input_price_per_k = st->models[i].input_cost_per_k;
        rules[idx].output_price_per_k = st->models[i].output_cost_per_k;
        idx++;
    }
    return rules;
}

/**
 * @brief Load pricing rules from a YAML model config (model.yaml).
 *
 * 复用 svc_yaml 解析状态机，将 models[].{name, input_cost_per_1k,
 * output_cost_per_1k} 转换为 pricing_rule_t（model_pattern=模型名精确
 * 匹配）。未声明价格的模型不产生规则，由 cost_tracker 落默认价。
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

    pricing_rel_models(&st);

    /* 注意：此处不调用 svc_yaml_expand_llm —— llm 简化段只有模型名没有
     * 价格，若替换 models 会把价格全丢。价格规则覆盖完整 models 列表，
     * 名字匹配到 llm 段扩展出的模型同样能查到价（SSoT：价格只定义在
     * models[].input/output_cost_per_1k）。 */

    int n = 0;
    for (size_t i = 0; i < st.model_count; ++i) {
        if (pricing_has_price(&st.models[i]))
            n++;
    }
    if (n == 0)
        return 0;

    pricing_rule_t *rules = pricing_fill_rules(&st, n);
    if (!rules)
        return 0;
    *out_rules = rules;
    *out_count = n;
    SVC_LOG_INFO("C-L02: SVC: pricing rules from YAML models=%d", n);
    return 0;
}
