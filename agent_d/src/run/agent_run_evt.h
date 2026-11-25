/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_evt.h
 * @brief run_stream 事件信封面：推送 sink 与 §2.4 v1 信封组装。
 *
 * 引擎、工具循环与委派机制件共用同一条事件通道；本头只承载通道契约，
 * 不引用会话注册表与循环产物类型，使各域可以只依赖通道而不互相拉入。
 */

#ifndef AIRY_RT_DAEMON_AGENT_RUN_EVT_H
#define AIRY_RT_DAEMON_AGENT_RUN_EVT_H

#include <stdint.h>

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief run_stream 事件推送 sink（M1-1d 协议先行）。
 *
 * 引擎在 run_start / plan / tool_start / tool_end / token_delta /
 * message / run_end / error 节点调用 emit，把结构化事件信封推送出去。
 * emit 回调不得阻塞（写 socket 失败即丢弃）；ud 为回调上下文。
 */
typedef struct {
    void (*emit)(const char *type, cJSON *data, void *ud);
    void *ud;
} agent_run_event_sink_t;

/**
 * @brief 组装 §2.4 v1 事件信封 JSON 并交给 sink->emit（agent_run_engine.c）。
 * data 与信封所有权均由本函数持有；sink 仅在调用期内借用（不得留存）。
 * emit 不得阻塞。run_id 为本次运行标识（每帧携带，可 NULL）。
 */
void agent_run_emit_event(const agent_run_event_sink_t *sink, uint64_t *seq,
                          const char *run_id, const char *session_id, const char *type,
                          cJSON *data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_RUN_EVT_H */
