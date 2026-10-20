/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief llm_d rpc 域私有：service 实例结构（struct llm_service）声明。
 */

#ifndef AIRY_RT_LLM_RPC_SERVICE_H
#define AIRY_RT_LLM_RPC_SERVICE_H

#include "cache_common.h"
#include "cost_tracker.h"
#include "llm_service.h"
#include "daemon_platform_ext.h"
#include "providers/core/registry.h"
#include "rpc/service_chain.h"
#include "svc_model_watch.h"

struct llm_service {
    provider_registry_t *registry;
    cache_t cache;
    cost_tracker_t *cost;
    airy_mtx_t lock;
    void *rules;
    size_t rule_count;
    char default_model[128];
    char default_provider[64]; /* global.default_provider */
    /* 引擎级输出上限（model.yaml llm.max_output / models[0].max_output 的
     * token 数，0 = 未配置）。请求未指定 max_tokens 且模型未在 provider
     * 侧声明上限时的兜底值，使配置面写下的上限真正生效。 */
    int default_max_output_tokens;
    /* 0.1.19 §3.5 缺陷5：启动快照（manager 配置 + 用户覆盖 model.yaml）。
     * 请求侧只检测不重载——漂移一次性 WARN + 落 stale 标记，由启动器
     * status 面提示重启。 */
    svc_mw_t mw;
    svc_mw_t mw_usr;
    /* 0.1.19 §3.8.4：调用级链式切换配置。缺省 off，现网选路行为不变；
     * 由 model.yaml routing 段填充（A4），CALLOC 全零不等于缺省，故
     * create 时必须经 svc_chain_def() 显式初始化。 */
    llm_route_cfg_t route;
};

/* 2.1.1.5 修复：计费/用量持久化文件路径（$AIRY_DATA_DIR/agentrt/
 * llm_usage.json），service 内部各模块共享（create 加载 / 每次真实调用
 * 后兜底保存 / destroy 保存）。 */
const char *llm_usage_state_path(void);

#endif /* AIRY_RT_LLM_RPC_SERVICE_H */
