/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief llm_d rpc 域私有：service 实例结构（struct llm_service）声明。
 */

#ifndef AIRY_RT_LLM_RPC_SERVICE_H
#define AIRY_RT_LLM_RPC_SERVICE_H

#include <stdint.h>

#include "cache_common.h"
#include "cost_tracker.h"
#include "llm_service.h"
#include "daemon_platform_ext.h"
#include "providers/core/registry.h"
#include "rpc/service_chain.h"
#include "svc_model_watch.h"
#include "svc_swap.h"

/* 排空超时后旧注册表仍有在途请求持有 provider 句柄，必须延后回收；
 * 宽限期取冷启动慢推理的量级，避免刚切换就释放正在使用的连接。 */
#define LLM_RETIRED_MAX 4
#define LLM_RETIRED_GRACE_MS 600000U

/* 热重载失败后的退避窗口：配置可能正被编辑，连续请求不应反复解析。 */
#define LLM_RELOAD_BACKOFF_MS 30000U

typedef struct {
    provider_registry_t *registry;
    pricing_rule_t *rules;
    int rule_count;
    uint64_t retire_ms;
} llm_retired_t;

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
     * 0.1.20 D2-10 起漂移不再只提示：请求侧检出即触发重载，快照随重载
     * 刷新。 */
    svc_mw_t mw;
    svc_mw_t mw_usr;
    /* 0.1.19 §3.8.4：调用级链式切换配置。缺省 off，现网选路行为不变；
     * 由 model.yaml routing 段填充（A4），CALLOC 全零不等于缺省，故
     * create 时必须经 svc_chain_def() 显式初始化。 */
    llm_route_cfg_t route;

    /* 0.1.20 D2-10：排空闸门与世代。配置字段只在 begin…commit 内替换，
     * 请求只在 enter…leave 内读取，两侧同锁故无撕裂。 */
    svc_swap_t swap;
    airy_mtx_t reload_lock; /* 单飞：同一时刻只有一个重载在执行 */
    uint64_t reload_next_ms;
    uint32_t reload_ok;
    uint32_t reload_fail;
    char cfg_path[1024];
    /* 排空超时的旧配置退场区（有界，最旧先回收） */
    llm_retired_t retired[LLM_RETIRED_MAX];
    int retired_n;
};

/* 计费/用量持久化路径（$AIRY_DATA_DIR/agentrt/llm_usage.json）：
 * create 加载、每次真实调用后兜底保存、destroy 保存。 */
const char *llm_usage_state_path(void);

/**
 * @brief 按当前已落盘配置重新装配服务（0.1.20 D2-10）。
 *
 * 调用方为请求侧漂移检测或管理面显式触发。返回前要么新配置已整体生效
 * 且世代 +1，要么旧配置原样继续服务（失败退避 30s）。
 *
 * @return 0 已切换；1 已有重载在执行或处于退避窗口；非 0 失败
 */
int llm_service_reload(llm_service_t *svc);

#endif /* AIRY_RT_LLM_RPC_SERVICE_H */
