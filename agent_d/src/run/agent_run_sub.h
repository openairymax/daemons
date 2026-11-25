/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_sub.h
 * @brief 子任务委派机制件（N-8）：把 subagent.spawn 接到 agent.run 工具循环。
 *
 * 一个委派是一次「子 run」：独立上下文（不重放父历史）、独立工具循环、
 * 共享父会话槽位（取消可下探）与父事件通道（帧带 sub_id/depth 还原树）。
 * 生命周期走 coreloopthree 的 delegate 原语：create → assign（执行子 run，
 * 预算耗尽时以 WOULD_BLOCK 续跑）→ collect → destroy；取消经
 * airy_delegate_cancel 落地，不是靠外层标志位假装。
 *
 * 配额（深度/并发/预算）全部来自策略面 agent_sub_policy.h；本文件没有
 * 任何「上限 = 常量」的判断。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_SUB_H
#define AIRY_RT_DAEMON_AGENT_RUN_SUB_H

#include <stdint.h>

/* 只借事件通道（帧归组用）；会话类型仅作前置声明，不把引擎域头拉进委派层 */
#include "agent_run_evt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct agent_run_session_s;

/* 子任务标识上界：随帧下发，客户端按它把子代理的步骤归到同一棵子树。 */
#define AGENT_SUB_ID_LEN 40

/** 委派入参（父循环的当前事实，全部借用，不持有）。 */
typedef struct {
    const char *args;                 /* tool_call arguments JSON（goal/context/budget） */
    const char *model;                /* 父 run 生效模型（子任务同模型） */
    const struct agent_run_session_s *session; /* 父会话（取消下探用） */
    const agent_run_event_sink_t *sink; /* 父事件通道（可 NULL） */
    uint64_t *seq;                    /* 父事件序号计数器（可 NULL） */
    const char *run_id;               /* 父 run 标识（可 NULL） */
    const char *sess_id;              /* 父会话标识（可 NULL） */
    uint64_t token_cap;               /* 父 run 生效 token 预算（0 = 未知） */
    int allow_dangerous;              /* 高危工具放行（透传子 run） */
} agent_sub_req_t;

/** 委派产物：text 由调用方 AIRY_FREE；用量并入父 run 账本。 */
typedef struct {
    char *text;
    uint64_t tokens;
    uint64_t tokens_in;
    uint64_t tokens_out;
    uint64_t llm_ms;
    uint64_t tool_ms;
    double cost;
} agent_sub_out_t;

/**
 * @brief 执行一次委派，把结论写入 out->text。
 *
 * 无论成功失败都尽量回传可读文本——工具失败是模型可观测的事实，不是
 * 进程级异常；配额拒绝同样以文本回传（并置错误码），让模型换路。
 *
 * @return 0 成功；AIRY_ECANCELLED 父任务取消；AIRY_EBUSY 配额拒绝或子 run 失败。
 */
int sub_task_run(const agent_sub_req_t *q, agent_sub_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_SUB_H */
