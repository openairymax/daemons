/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_sub_policy.h
 * @brief 子任务委派策略面（N-8）：配额解析、线程内深度与工具契约。
 *
 * 机制与策略分离：本文件只回答「允许到什么程度」——深度/并发配额从哪里
 * 取、当前调用链已用到第几层、委派配额还剩多少；子任务怎么跑（会话、
 * 预算、事件、取消）由 agent_run_sub.c 的机制件承担。配额不是编译期
 * 常量：部署配置（subagent.max_depth / subagent.max_concurrent）优先，
 * 环境变量次之，最后才是缺省值，且一律夹到硬上界以内。
 *
 * 深度是「调用链」属性而不是「进程」属性：同一进程内多个会话可并行委派，
 * 各自链条互不相干，故用线程局部计数（agent.run 每请求一线程）。
 */

#ifndef AIRY_RT_DAEMON_AGENT_SUB_POLICY_H
#define AIRY_RT_DAEMON_AGENT_SUB_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/* 工具名：模型可见的委派入口。Provider 的 function.name 只接受
 * ^[a-zA-Z0-9_-]+$（点号会使整套 tools 被 400 拒收），故用下划线。 */
#define AGENT_SUB_TOOL "subagent_spawn"

/* 配额缺省与硬上界。缺省值即常见部署的合理值：两层委派、四个并行子任务
 * 足以覆盖「主任务拆两三个独立子问题」的绝大多数场景，再深就退化为串行
 * 思考，收益抵不上 token 成本。上界防止配置误填把守护进程拖垮。 */
#define AGENT_SUB_DEPTH_DEFAULT 2
#define AGENT_SUB_DEPTH_CAP 8
#define AGENT_SUB_CONC_DEFAULT 4
#define AGENT_SUB_CONC_CAP 64

/* 策略面覆盖通道：未在部署配置里显式给值时从这里取。 */
#define AGENT_SUB_ENV_DEPTH "AIRY_SUBAGENT_MAX_DEPTH"
#define AGENT_SUB_ENV_CONC "AIRY_SUBAGENT_MAX_CONCURRENT"

/* 未显式给预算时子任务拿父预算的这个份额；下限避免父预算很小时子任务
 * 连一次工具往返都跑不完。 */
#define AGENT_SUB_BUDGET_DIV 3
#define AGENT_SUB_BUDGET_MIN 4096ULL

/** @brief 生效配额（均已夹到硬上界以内）。 */
typedef struct {
    int max_depth;
    int max_conc;
} agent_sub_limits_t;

/** @brief 部署配置注入（<=0 表示未配置，回落到环境变量/缺省值）。 */
void sub_limits_set(int max_depth, int max_conc);

/** @brief 解析生效配额：配置 > 环境变量 > 缺省，逐项夹紧。 */
agent_sub_limits_t sub_limits(void);

int sub_is_tool(const char *name); /* 委派工具判据：全等，本判据只此一处 */

/** @brief 当前线程调用链已处于第几层委派（顶层 run 为 0）。 */
int sub_depth(void);

/** @brief 进入一层委派：0 成功；1 已达深度上限（未进入）。 */
int sub_enter(void);

/** @brief 退出一层委派（与 sub_enter 成对，容忍多余调用）。 */
void sub_leave(void);

/** @brief 占用一个并发槽位：1 成功；0 已满（未占用）。 */
int sub_slot_try(void);

/** @brief 释放并发槽位（容忍多余调用）。 */
void sub_slot_drop(void);

/** @brief 委派工具的一句话描述（模型侧说明）。 */
const char *sub_desc(void);

/** @brief 委派工具的 parameters JSON（cJSON_Parse 消费，调用方持有）。 */
const char *sub_schema(void);

/** @brief 子任务提示词前缀（策略面文案，机制件只做拼接）。 */
const char *sub_lead(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_SUB_POLICY_H */
