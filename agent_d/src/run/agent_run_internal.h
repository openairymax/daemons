/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_internal.h
 * @brief Agent run engine 内部共享声明（模块私有，不对外导出）。
 *
 * agent_run_engine.c（会话注册表 + spec 解析 + 编排 + 主入口）与
 * agent_run_loop.c（工具循环 ReAct）之间的共享契约；工具循环域的入参/
 * 产物/策略解析单立在 agent_run_loop.h，事件通道在 agent_run_evt.h，
 * 使循环与委派不必反向依赖引擎域头。RPC 适配层只依赖
 * agent_run_engine.h 的公共接口。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "agent_run_engine.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 上下文台账域（agent_run_ledger.c） ---- */

#define AGENT_LEDGER_ID_LEN 33   /* mem_d entry_id：32 hex + NUL（ledger.h） */
#define AGENT_LEDGER_TYPE_LEN 12 /* 入口类型字符串上界（含 NUL） */

/** 台账条目 ↔ 消息映射（entry_id 为空表示已压缩的摘要占位）。 */
typedef struct {
    char entry_id[AGENT_LEDGER_ID_LEN];
    char type[AGENT_LEDGER_TYPE_LEN];
    cJSON *msg; /* messages 数组内节点引用（非拥有） */
} agent_ledger_item_t;

/** 会话台账客户端状态：mem_d socket + 记账映射（渐进降级，disabled 时不记账）。 */
typedef struct {
    int enabled;
    char sock[AIRY_PATH_MAX];
    char sess[AGENT_RUN_SESSION_ID_LEN];
    agent_ledger_item_t *items;
    size_t count;
    size_t cap;
} agent_ledger_t;

/**
 * @brief 初始化台账客户端（sess 为空或 socket 解析失败时置 disabled）。
 * @return 恒为 0（enabled 状态经结构体表达）
 */
int agent_ledger_init(agent_ledger_t *lg, const char *sess);

/** @brief 释放映射数组（不拥有 messages 节点，无需释放引用）。 */
void agent_ledger_free(agent_ledger_t *lg);

/**
 * @brief 记账一条消息：ledger_append 落库并建立 entry_id ↔ msg 映射。
 * 失败静默（渐进降级），不阻断主对话。
 */
void agent_ledger_add(agent_ledger_t *lg, const char *type, const char *text, size_t token_out,
                      cJSON *msg);

/**
 * @brief 每轮 LLM 调用前预算适配：ledger_window warn 时对候选区
 * （ReAct 轮次成组，入口 user 与末组受保护）执行 mem.compress，
 * 用压缩上下文原位替换本地消息组。
 */
void agent_ledger_fit(agent_ledger_t *lg, cJSON *messages);

/* ---- 计量域（agent_run_ledger.c） ---- */

/**
 * 用量入参（S30-P0-5）：token 为 provider 实报分量，*_bytes 仅用于
 * provider 未实报时按 4 字节/token 估算；cost 恒透传。
 */
typedef struct {
    uint64_t total;
    uint64_t prompt;
    uint64_t completion;
    size_t prompt_bytes;
    size_t text_bytes;
    double cost;
} agent_usage_in_t;

/**
 * @brief 构建统一 usage 对象（prompt/completion/total/cost/source）。
 *
 * result、run_end 与决策链 run_result 共用本函数，是全项目唯一的计量
 * 载荷形状；调用方取得所有权（cJSON_Delete）；OOM 返回 NULL。
 */
cJSON *agent_usage_obj(const agent_usage_in_t *in);

/* ---- 流式域（agent_run_stream.c） ---- */

#define AGENT_STREAM_ERR_LEN 256 /* 流错误文案上界（含 NUL） */

/** 增量正文回调：delta 非 NUL 结尾，长度由 dlen 给出；不得阻塞。 */
typedef struct {
    void (*on_text)(const char *delta, size_t dlen, void *ud);
    void *ud;
} agent_stream_sink_t;

/** 一条流消费完的产物（所有权移交调用方）。 */
typedef struct {
    char *text;        /* 正文（AIRY_* 分配，恒非 NULL） */
    char *reason;      /* 思考链（无帧时为 NULL） */
    cJSON *tools;      /* tool_calls 数组（AIRY/cJSON 分配，无帧时为 NULL） */
    uint64_t tokens;   /* 'U' 帧用量 */
    uint64_t prompt_tokens;     /* 'U' 帧 prompt 分量，未报为 0 */
    uint64_t completion_tokens; /* 'U' 帧 completion 分量，未报为 0 */
    double cost;       /* 'U' 帧费用 */
    int error_code;    /* 'E' 帧错误码；0 表示无错误帧 */
    int upstream_code; /* 'E' 帧 data.err 的真实上游码；0 表示未携带（如参数拒绝） */
    char error_msg[AGENT_STREAM_ERR_LEN];
} agent_stream_result_t;

/**
 * 流式解复用状态（RS 分帧跨 payload 有状态）。
 * 帧协议权威见 commons/include/airy_llm_stream.h。
 */
typedef struct {
    agent_stream_sink_t sink;
    char *text;          /* 正文聚合（恒 NUL 结尾） */
    size_t text_len;
    size_t text_cap;
    size_t text_sent;    /* 已推送字节数（UTF-8 对齐后前缀） */
    char *reason;        /* 思考链聚合（恒 NUL 结尾） */
    size_t reason_len;
    size_t reason_cap;
    char *frame;         /* 帧体聚合（恒 NUL 结尾） */
    size_t frame_len;
    size_t frame_cap;
    char frame_tag;      /* 非 0 表示正在收帧 */
    size_t frame_dropped;/* 超限/ OOM 丢弃字节数（>0 即整帧废弃） */
    int prev_rs;         /* 上一 payload 以裸 RS 结尾，判定挂起 */
    cJSON *tools;
    uint64_t tokens;
    uint64_t prompt_tokens;
    uint64_t completion_tokens;
    double cost;
    int error_code;
    int upstream_code;
    char error_msg[AGENT_STREAM_ERR_LEN];
} agent_stream_t;

/** @brief 初始化（sink 可 NULL，表示只聚合不推送）。 */
void agent_stream_init(agent_stream_t *st, const agent_stream_sink_t *sink);

/** @brief 释放全部内部缓冲与工具数组。 */
void agent_stream_free(agent_stream_t *st);

/** @brief 喂入一段原始流字节（可任意切分）。 */
void agent_stream_feed(agent_stream_t *st, const char *data, size_t len);

/** @brief daemon_rpc_stream_cb_t 适配器：ud 为 agent_stream_t*。 */
void agent_stream_on_chunk(const char *data, size_t len, void *ud);

/** @brief 流结束：残缺帧告警、挂起 RS 透出、冲刷 UTF-8 回撤段。 */
void agent_stream_finish(agent_stream_t *st);

/** @brief 取出产物并复位源状态（text 所有权移交，空文本给 ""）。 */
void agent_stream_take(agent_stream_t *st, agent_stream_result_t *out);

/* ---- run 会话域（agent_run_engine.c） ---- */

/** @brief 注册一次 run 的会话（同名复用；OOM 返回 NULL）。 */
agent_run_session_t *agent_run_register(const char *session_id);

/** @brief 注销会话并释放（active 置 NULL 后调用）。 */
void agent_run_unregister(agent_run_session_t *s);

/** @brief 生成会话 ID（sess_<16hex>_<4hex>）。 */
void agent_run_gen_session_id(char *out, size_t out_size);

/** @brief 生成运行 ID（run_<16hex>_<4hex>）。 */
void agent_run_gen_run_id(char *out, size_t out_size);

/** @brief 在途回合数（D-12 退出排水观测：注册表条目在 run 结束才注销）。 */
size_t run_active_cnt(void);

/** @brief 排水兜底：全部在途会话置取消，返回受影响数。 */
int agent_run_cancel_all(void);

/* ---- 编排域（agent_run_engine.c） ---- */

/**
 * @brief 编排分支：进程内 spawn+invoke（agent.run 单入口，无 RPC 环）。
 *
 * @param agent_spec params.agent 对象（非 NULL）
 * @param prompt     invoke 输入
 * @param out_text   输出文本（AIRY_* 分配，调用方 AIRY_FREE）
 * @param out_err    失败原因（AIRY_* 分配；成功为 NULL）；文本形如
 *                   "agent.<stage> failed: <ERR_SYMBOL> (<rc>)"，是调用方
 *                   唯一的失败判读入口，不得替换为通用话术
 * @return 0 成功；非零失败
 */
int agent_run_orchestrate(const cJSON *agent_spec, const char *prompt, char **out_text,
                          char **out_err);

/* ---- 决策链写侧（agent_run_exec.c） ---- */

/**
 * @brief 判定一行待落盘的链事件是否可持久化：UTF-8 合法且 JSON 可解析。
 *
 * 链事件按 NDJSON 追加，一条坏行会让整段回放失败，故写入前必须过此判定；
 * 失败由调用方告警并丢弃（best effort，不阻塞 run 主流程）。
 */
int agent_evt_ok(const char *line);

/* ---- 记忆召回（agent_run_mem.c） ---- */

/**
 * @brief 取本会话记忆上下文（最近记录 + 检索命中），拼为 system 正文。
 *
 * 范围按 conv 收窄到当前会话；mem_d 不可达、无记录或全部命中重复时
 * 返回 NULL，调用方按「无记忆」继续本轮。
 *
 * @param sess [in] 会话 ID（BORROW）。
 * @param prompt [in] 本轮用户输入（BORROW）。
 * @return 堆字符串（TRANSFER，调用方 AIRY_FREE）；无内容为 NULL。
 */
char *run_mem_recall(const char *sess, const char *prompt);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_INTERNAL_H */
