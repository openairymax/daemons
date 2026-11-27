/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_loop.h
 * @brief ReAct 工具循环域：入参/产物契约、轮次与预算解析、失败语义分类。
 *
 * 循环只借用会话的取消位与事件通道，不拥有会话注册表；会话类型在此仅作
 * 前置声明，避免循环域反过来依赖引擎域头。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_LOOP_H
#define AIRY_RT_DAEMON_AGENT_RUN_LOOP_H

#include <stdbool.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "agent_run_evt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct agent_run_session_s;

/* 工具环轮次策略（N-7）：默认不设轮次上限——运行持续到任务完成，停止只由
 * 三个安全阀之一触发：同参重复失败、token 预算越限、取消。客户端
 * params.rounds / budget.rounds 为正数时按其显式上限收口，策略面
 * AIRY_AGENT_TOOL_ROUNDS 亦可显式收紧；不存在 32 之类的机制硬界。
 * 到点是暂停不是崩溃：已执行 trace 与中间正文完整移交，并返回 AIRY_EBUDGET
 * + resume_token 供客户端续跑。 */
#define AGENT_RUN_ROUNDS_AUTO 0

/** 工具循环入参（契约 §6：超 4 参数以结构体封装）。 */
typedef struct {
    const char *prompt;                     /* 用户输入（history 为空时构造首条消息） */
    const cJSON *history;                   /* OpenAI messages 数组（可 NULL） */
    const char *model;                      /* 模型名（非 NULL） */
    const struct agent_run_session_s *session; /* 会话（轮间取消检查；可 NULL） */
    const agent_run_event_sink_t *sink;     /* run_stream 事件推送 sink（可 NULL） */
    int rounds;                             /* 请求轮次上限（<=0 = 不设上限，N-7） */
    uint64_t token_budget;                  /* 请求 token 预算（0 = 取策略/机制缺省） */
    const char *resume_token;               /* 续跑令牌（可 NULL；非空时不重放全量历史） */
    bool allow_dangerous;                   /* 高危工具放行声明（透传 tool_d 审批门） */
    /* 授权主体：代理档案自报 id（如 coding_v1），未署名时留空——tool_d 据此
     * 回落 "tool_d" 并沿用既有 ACL 语义。不得填会话实例号：ACL 里没有实例
     * 条目，填了会让每一次工具调用都 fail-closed。 */
    const char *agent_id;
    /* 审计主体（V-05）：命名代理 id 优先，缺省回退会话 id，只进台账留痕，
     * 不参与 ACL/审批判定。两侧分账，审计五字段（task/agent/tool/argv/
     * verdict）才既完整又不改变授权结果。 */
    const char *audit_id;
    /* 事件序号外部化（N-8）：嵌套 run（子任务）与父 run 共用一条序号，
     * 调用方传入父计数器即可，NULL 则本 run 自持。 */
    uint64_t *seq_ext;
} agent_run_loop_args_t;

/** 工具循环产物（成功/熔断/预算耗尽时所有权移交调用方）。 */
typedef struct {
    cJSON *trace;      /* 工具 trace 数组（调用方 cJSON_Delete）；无工具时为 NULL */
    char *text;        /* 最终回复文本（AIRY_* 分配，调用方 AIRY_FREE） */
    char *reason;      /* 累计思考链（AIRY_* 分配，可 NULL） */
    char *resume_token;/* 预算耗尽时生成的续跑令牌（AIRY_* 分配，其余 NULL） */
    uint64_t tokens;   /* 累计 token */
    uint64_t tokens_in;  /* 累计 prompt token（provider 实报，未报为 0） */
    uint64_t tokens_out; /* 累计 completion token（provider 实报，未报为 0） */
    double cost;       /* 累计成本 */
    uint64_t llm_ms;   /* LLM 段累计耗时 */
    uint64_t tool_ms;  /* 工具段累计耗时 */
    int rounds_used;   /* 本轮实际消耗轮次 */
    int rounds_limit;  /* 本轮生效轮次上限（0 = 不设上限） */
    /* 停止原因（N-7）：AIRY_RS_STOP_DONE / CANCELLED / ROUNDS 或
     * AIRY_RS_REASON_*，静态串，不拥有内存。 */
    const char *stop_reason;
    char fail_node[128]; /* 熔断工具 id（D-10 affected_node），无熔断为空 */
    int fail_denied;     /* D-10：失败前出现过审批/策略拒绝 */
    /* 上游失败明细（LLM 传输错误 / 'E' 帧码与文案）：终态错误帧优先采用，
     * 否则整条链路只剩一句兜底文案，客户端无法归类。 */
    char fail_msg[256];
    int fail_transport; /* 上游失败是否为传输层（决定是否可对用户说「不可达」） */
    int fail_code;      /* 'E' 帧携带的真实上游码（0 = 未携带，仅有协议码） */
} agent_run_loop_result_t;

/** D-10 失败语义分类（纯函数，无分配）。 */
typedef struct {
    const char *origin;      /* AIRY_RS_ORIGIN_* */
    const char *reason;      /* AIRY_RS_REASON_* */
    int retryable;           /* 原样重试是否安全 */
    const char *next_action; /* AIRY_RS_NEXT_* */
} agent_fail_desc_t;

/** @brief 按原始退出码分类失败（纯函数，无分配）。 */
agent_fail_desc_t agent_fail_desc(int rc, int denied);

/** @brief 轮次上限解析：请求 > 策略（AIRY_AGENT_TOOL_ROUNDS）> 0（不设上限）。 */
int rounds_resolve(int requested);

/** @brief token 预算解析：请求 > 策略（AIRY_AGENT_TOKEN_BUDGET，0=不限）
 *         > 机制缺省 AGENT_RUN_TOKENS_DEFAULT。 */
uint64_t tokens_resolve(uint64_t requested);

/**
 * @brief ReAct 工具循环：LLM complete_stream -> 增量上屏 -> tool_calls ->
 *        tool_d 执行 -> 回填 -> 继续。
 *
 * @param args 入参（非 NULL）
 * @param out  产物（非 NULL；成功与熔断时按上述所有权移交）
 * @return 0 成功；1 用户取消；AGENT_RUN_RC_TOOL_FUSE 熔断；
 *         AIRY_EBUDGET 轮次预算耗尽（产物照常移交）；负值失败
 */
int agent_run_tool_loop(const agent_run_loop_args_t *args, agent_run_loop_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_LOOP_H */
