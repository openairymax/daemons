/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file provider_rec.h
 * @brief registry 装配记录 provider_t（0.1.20 §7.5 扇入收敛）。
 *
 * adapter.h 是适配器「实现契约」（五纯函数槽 + driver 入口），本头是
 * registry 的「装配产物」（身份 + 适配器挂点 + 每模型预算）。二者消费方
 * 不同：选路与链式切换只读记录，适配器只实现契约。记录拆出后，registry
 * 与切换面不再为取得 provider_t 而整头引入适配器契约。
 *
 * 记录依赖契约（provider_t 持 provider_adapter_t 指针），故本头向下
 * include adapter.h，反向不成立。
 */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_REC_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_REC_H

#include "adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const provider_adapter_t *adapter;
    provider_ctx_t *ctx;
    char **models;
    /* 与 models 同下标对齐的每模型输出上限（0 = 未配置）：registry 装配时
     * 从 provider_config_t 复制，供生成参数解析查询。 */
    int *model_max_output;
    /* 与 models 同下标对齐的每模型调用方并发预算（D2-9④，0 = 未配置）。 */
    int *model_max_concurrent;
} provider_t;

/* 查 provider 对某模型的调用方并发预算（0 = 未配置）。纯 provider_t 表查询，
 * 不触及 registry 状态，故归位属主域此处。 */
int prov_max_conc(const provider_t *prov, const char *model);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_REC_H */
