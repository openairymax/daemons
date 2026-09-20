/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief Internal service structure declarations.
 */

#ifndef AIRY_RT_LLM_SERVICE_INTERNAL_H
#define AIRY_RT_LLM_SERVICE_INTERNAL_H

#include "cache.h"
#include "cost_tracker.h"
#include "llm_service.h"
#include "daemon_platform_ext.h"
#include "providers/core/registry.h"
#include "token_counter.h"

struct llm_service {
    provider_registry_t *registry;
    llm_cache_t *cache;
    cost_tracker_t *cost;
    token_counter_t *token_counter;
    airy_mtx_t lock;
    void *rules;
    size_t rule_count;
    char default_model[128];
    char default_provider[64]; /* global.default_provider */
    /* 引擎级输出上限（model.yaml llm.max_output / models[0].max_output 的
     * token 数，0 = 未配置）。请求未指定 max_tokens 且模型未在 provider
     * 侧声明上限时的兜底值，使配置面写下的上限真正生效。 */
    int default_max_output_tokens;
};

/* 2.1.1.5 修复：计费/用量持久化文件路径（$AIRY_DATA_DIR/agentrt/
 * llm_usage.json），service 内部各模块共享（create 加载 / 每次真实调用
 * 后兜底保存 / destroy 保存）。 */
const char *llm_usage_state_path(void);

#endif /* AIRY_RT_LLM_SERVICE_INTERNAL_H */