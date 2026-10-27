/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service_boot.h
 * @brief 配置读入与落盘的中间态（0.1.20 D2-10 热切换的暂存面）。
 *
 * 创建与热重载共用同一条读配置路径：先全部读进 llm_boot_t，成功后再一次性
 * 落到 svc。迁移前创建路径边读边写 svc，reload 无法在中途失败而保持旧配置，
 * 也无法在排空窗口内一次性换表。
 *
 * 语义约定：
 *   - fill 只读外存、不触碰 svc；失败信息落在 load_err，供重载方决定放弃
 *   - apply 只在排空窗口内调用，且接管 rules 所有权（调用后 boot 不再持有）
 *   - drop 幂等，可在 fill 之后任一时刻调用
 */

#ifndef AIRY_RT_LLM_RPC_SERVICE_BOOT_H
#define AIRY_RT_LLM_RPC_SERVICE_BOOT_H

#include <stddef.h>

#include "cost_tracker.h"
#include "providers/core/provider_config.h"
#include "rpc/service_chain.h"
#include "svc_model_defaults.h"

#ifdef __cplusplus
extern "C" {
#endif

struct llm_service;

typedef struct {
    service_config_t base;
    provider_config_t *provs;
    size_t prov_n;
    char model[128];
    char pname[64];
    svc_model_llm_config_t llm;
    const char *src;
    pricing_rule_t *rules; /* 配置侧价格表，drop 释放；apply 后所有权转 svc */
    int rule_count;
    llm_route_cfg_t route;
    int load_err; /* 1 = 主配置读失败或为空，调用方不得据此替换在线配置 */
} llm_boot_t;

/**
 * @brief 用户覆盖文件 $AIRY_CONFIG_DIR/model.yaml 存在时返回 1 并写出路径。
 */
int llm_boot_usr_path(char *buf, size_t n);

/**
 * @brief 读取配置到暂存态（缺省值 + 主配置 + 用户覆盖合并）。
 *
 * @param b   出参，函数内部先整体清零
 * @param cfg 主配置路径，可为 NULL（仅用户覆盖与缺省生效）
 */
void llm_boot_fill(llm_boot_t *b, const char *cfg);

/** @brief 释放暂存态持有的数组与规则表 */
void llm_boot_drop(llm_boot_t *b);

/**
 * @brief 暂存态落到 svc（默认模型/提供方/输出上限、路由、价格表）。
 *
 * 调用方须已进入排空窗口；rules 所有权在返回后归 svc。
 */
void llm_boot_apply(struct llm_service *svc, llm_boot_t *b);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_RPC_SERVICE_BOOT_H */
