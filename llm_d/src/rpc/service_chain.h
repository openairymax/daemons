/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service_chain.h
 * @brief 调用级链式切换机制：候选重选、跳数与预算约束（0.1.19 §3.8.4 A3）。
 *
 * 切换分三件看：端点健康面回答「谁坏了」，决策件回答「这个错误值不值得
 * 换」，本件回答「换了打谁、还能换几次、还剩多少时间」。三者都不持有
 * 传输细节，因此本件以 attempt 闭包接收真实调用，使机制可在单元测试中
 * 注入替身而不引入任何生产桩。
 *
 * 三条纪律：
 *   1. mode=off 时健康面仍记账（D2-2），但放行闸、候选枚举与预算全部旁路，
 *      调用侧逐字节维持 0.1.18 选路行为；
 *   2. 全链失败返回首个原始错误码，不得笼统化为「无可用模型」。上游需要
 *      区分密钥错误与网络错误，笼统化会让排障与重试策略一起失效；
 *   3. 每次切换以实际模型重跑生成参数解析，并回填 svc_hit_t，供计费与
 *      语义缓存按实际模型记账（同一逻辑请求换端点后模型可能不同）。
 */

#ifndef AIRY_RT_LLM_RPC_SERVICE_CHAIN_H
#define AIRY_RT_LLM_RPC_SERVICE_CHAIN_H

#include "airy_abi.h"
#include "llm_service_types.h"
#include "providers/core/provider_rec.h"
#include "router/core/llm_router.h"

AIRY_ABI_BEGIN

/** routing.mode 取值。缺省 off：现网选路行为不变。 */
#define SVC_CHAIN_OFF 0
#define SVC_CHAIN_AUTO 1

/** 缺省最大切换跳数（不含首端点）。 */
#define SVC_CHAIN_DEF_DEPTH 2

/** 单请求尝试集合与候选枚举的共同上界，保证排除集合有界且必然终止。 */
#define SVC_CHAIN_MAX_TRIED 8

/** routing 段配置面（model.yaml），由 service.c 装配、svc_route_load 填充。 */
typedef struct {
    int mode;         /* SVC_CHAIN_OFF / SVC_CHAIN_AUTO */
    int depth;        /* 最多切换跳数 */
    unsigned on_mask; /* 可切换类白名单，LLM_FO_BIT() 聚合 */
    int budget_ms;    /* 单次 llm.call 总预算；routing.deadline_sec 秒 ×1000；<=0 不限 */
} llm_route_cfg_t;

/** 已尝试端点（provider + 实际模型）。 */
typedef struct {
    char prov[64];
    char model[64];
} svc_ep_t;

/** 单次请求的链状态。 */
typedef struct {
    llm_route_cfg_t cfg;
    uint64_t start_ms;
    int hops;
    int first_err;
    int tried_n;
    svc_ep_t tried[SVC_CHAIN_MAX_TRIED];
} svc_chain_t;

/** 一次尝试的输入：端点与实际生效的请求配置。 */
typedef struct {
    const provider_t *prov;
    llm_request_config_t eff;
} svc_step_t;

/** 一次成功尝试的落点：计费、截断提示与语义缓存都按实际模型记账。 */
typedef struct {
    const provider_t *prov;
    char model[128];
    int max_tokens;
} svc_hit_t;

/** 真实调用闭包：由调用侧绑定 adapter，机制不感知传输。 */
typedef int (*svc_attempt_fn)(void *ctx, const svc_step_t *step, llm_response_t **out);

/** 流式首字节哨兵：返回非 0 表示已向下游下发内容，禁止再切换。 */
typedef int (*svc_sent_fn)(void *ctx);

/** 填充缺省配置：off / 2 跳 / 三类可切换 / 不设预算。 */
void svc_chain_def(llm_route_cfg_t *cfg);

/**
 * 从 model.yaml routing 段覆盖 cfg 中已声明的键。
 *
 * cfg 须先由 svc_chain_def() 置缺省。本函数不重置未声明的键，故可对主配置
 * 与用户覆盖依次调用（后者显式键优先）。段缺失时 cfg 保持不变。
 *
 * @param path 配置文件路径。
 * @param cfg  待覆盖的配置。
 * @return AIRY_OK 已应用；AIRY_ERR_NOT_FOUND 无 routing 段；
 *         AIRY_ERR_IO 读取/解析失败；AIRY_ERR_INVALID_PARAM 参数非法或段类型错误。
 */
int svc_route_load(const char *path, llm_route_cfg_t *cfg);

/** 以配置初始化链状态；cfg 为 NULL 时取缺省。 */
void svc_chain_init(svc_chain_t *chain, const llm_route_cfg_t *cfg);

/** 调用级预算是否仍有余量（纯函数；预算不限时恒为 1）。 */
int svc_chain_left(const svc_chain_t *chain, uint64_t now);

/** 是否处于 auto 模式。 */
int svc_chain_auto(const svc_chain_t *chain);

/** 记入已尝试集合；集合满时静默丢弃，由 svc_chain_next 终止枚举。 */
void svc_chain_mark(svc_chain_t *chain, const char *prov, const char *model);

/**
 * 判定失败后是否继续切换，并记录首个错误码。
 * @return 1 继续，0 停止（白名单外 / 跳数用尽 / 预算耗尽）。
 */
int svc_chain_retry(svc_chain_t *chain, int err);

/**
 * 取下一个未尝试候选，命中即记入集合并消耗一跳。
 * @return 1 命中（prov/model 已填充），0 无候选或集合已满。
 */
int svc_chain_next(const llm_route_request_t *req, svc_chain_t *chain, char *prov, size_t psz,
                   char *model, size_t msz);

/**
 * 执行链式调用。
 *
 * @param svc     服务实例（取 routing 配置、注册表与生成参数解析）。
 * @param manager 调用方请求配置（意图）。
 * @param req     候选枚举用路由请求（与首选选路同源构造）。
 * @param first   首选端点，由调用侧选路得到。
 * @param attempt 真实调用闭包。
 * @param actx    闭包上下文。
 * @param sent    流式首字节哨兵，可为 NULL（同步路径）。
 * @param out     成功响应输出；为 NULL 时响应由本件释放（流式兼容）。
 * @param hit     实际落点输出，可为 NULL。
 * @return 0 成功；失败为首个原始错误码。
 */
int svc_chain_run(llm_service_t *svc, const llm_request_config_t *manager,
                  const llm_route_request_t *req, const provider_t *first, svc_attempt_fn attempt,
                  void *actx, svc_sent_fn sent, llm_response_t **out, svc_hit_t *hit);

/**
 * 把服务实例的 routing 配置按值取出（svc 为 NULL 时落默认档）。
 *
 * 本件据此与 struct llm_service 解耦：只经不透明指针透传，不自解引用。
 */
void svc_chain_ctx(const llm_service_t *svc, llm_route_cfg_t *out);

/** 注册表精确查找（持锁），未命中返回 NULL（实现在 service.c）。 */
const provider_t *svc_find_prov(llm_service_t *svc, const char *model);

/* 注册表为空（持锁计数，实现在 service.c）：与「模型未配置」分开归因，
 * 避免把装配故障报成不可重试的 INVALID_MODEL。 */
bool svc_reg_empty(llm_service_t *svc);

AIRY_ABI_END

#endif /* AIRY_RT_LLM_RPC_SERVICE_CHAIN_H */
