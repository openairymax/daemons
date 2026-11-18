/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_job.h
 * @brief C-11 长任务作业句柄：submit / poll / cancel 与作业记录落盘。
 *
 * 作业句柄把「工具执行」与「结果交付」解耦：客户端断开不再等于任务失败，
 * 结果写入 data/agentrt/jobs/<job_id>.json，重启后可读回历史记录。
 * 执行本身仍走 tool_service_execute（单一执行入口，权限/审批/沙箱不变），
 * 本模块只负责生命周期、状态机与持久化。
 */

#ifndef AIRY_TOOL_JOB_H
#define AIRY_TOOL_JOB_H

#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "tool_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_JOB_ID_MAX 48
#define TOOL_JOB_FIELD_MAX 64
#define TOOL_JOB_SUMMARY_MAX 256
#define TOOL_JOB_SLOTS 128 /* 内存中保留的作业条目上限（含已结束） */

/** @brief 作业状态机：running 是唯一运行态，其余均为终态。 */
typedef enum {
    TOOL_JOB_RUNNING = 0,
    TOOL_JOB_DONE,
    TOOL_JOB_FAILED,
    TOOL_JOB_CANCELLED,
    TOOL_JOB_TIMED_OUT,
} tool_job_state_t;

/** @brief 作业快照（poll 返回；output/error 由调用方 tooljob_rel 释放）。 */
typedef struct {
    char id[TOOL_JOB_ID_MAX];
    char tool_id[TOOL_JOB_FIELD_MAX];
    char agent_id[TOOL_JOB_FIELD_MAX];
    tool_job_state_t state;
    int exit_code;
    int err_code;
    int timeout_sec;
    uint64_t created_ms;
    uint64_t finished_ms;
    char *output;
    char *error;
    /* D-9 审计留痕（定长，随快照按值复制） */
    int risk;                    /* 0 = 普通，1 = 高危 */
    char risk_reason[TOOL_JOB_FIELD_MAX];
    char params_sum[TOOL_JOB_SUMMARY_MAX];
    char output_sum[TOOL_JOB_SUMMARY_MAX];
    char approval[TOOL_JOB_FIELD_MAX]; /* none|acl|interactive|allow_dangerous|warn */
    char sandbox[32];                  /* landlock|degraded|off */
} tool_job_info_t;

/** @brief 作业存储（不透明）。 */
typedef struct tooljob tooljob_t;

/**
 * @brief 执行回调：由 RPC 层注入，ctx 为其持有的服务句柄。
 * @return 0 且 *out 非空表示执行完成；非 0 表示执行层拒绝（*out 可为空）。
 */
typedef int (*tool_job_run_t)(void *ctx, const tool_execute_request_t *req,
                              tool_result_t **out);

/**
 * @brief 创建作业存储并加载 root 下的历史记录。
 * @param root 作业目录；NULL/空串取 tooljob_root()。
 * @param run  执行回调（必填）。
 * @param ctx  回调上下文。
 * @return 句柄；参数非法或目录不可建时返回 NULL。
 */
tooljob_t *tooljob_new(const char *root, tool_job_run_t run, void *ctx);

/** @brief 释放句柄；运行中的作业线程 detach 后继续完成并落盘。 */
void tooljob_free(tooljob_t *jobs);

/**
 * @brief 等待运行中作业收尾（守护退出前用，避免执行线程访问已销毁服务）。
 * @param timeout_ms 最长等待毫秒数
 * @return 0 = 全部结束；AIRY_ETIMEDOUT = 仍有作业运行；负值 = 参数错误
 */
int tooljob_drain(tooljob_t *jobs, int timeout_ms);

/**
 * @brief 提交作业（立即返回 job_id，执行在后台线程）。
 * @param timeout_sec 本次执行预算（秒）；<=0 用工具元数据缺省值。
 * @return 0 成功；AIRY_ERR_* 失败（未知工具、容量满、线程创建失败）。
 */
int tooljob_submit(tooljob_t *jobs, const tool_execute_request_t *req, int timeout_sec,
                   char *job_id_out, size_t cap);

/** @brief 读取作业快照；未知 job_id 返回 AIRY_ERR_NOT_FOUND。 */
int tooljob_poll(tooljob_t *jobs, const char *job_id, tool_job_info_t *out);

/**
 * @brief 同步执行落账：把一次已完成的 tool.execute 写成终态作业记录。
 *
 * D-9 要求 tool.execute 与 tool.submit 共用同一任务实体；同步路径没有
 * 后台线程，故直接写入终态（含参数摘要/风险/审批/沙箱），并返回 job_id
 * 供响应与 jobs show 关联。res 为 NULL 时返回参数错误。
 */
int tooljob_record(tooljob_t *jobs, const char *tool_id, const char *agent_id,
                   const char *params_json, const tool_result_t *res, uint64_t start_ms,
                   char *job_id_out, size_t cap);

/**
 * @brief 取消作业（协作式）：运行中标记为 cancelled 并作废结果；
 *        已结束作业返回 AIRY_ERR_INVALID_STATE。执行进程仍受 timeout 预算约束。
 */
int tooljob_cancel(tooljob_t *jobs, const char *job_id);

/** @brief 当前内存中的作业条目数（含终态）。 */
int tooljob_count(const tooljob_t *jobs);

/**
 * @brief 列出作业快照数组（最新在前），最多 limit 条。
 * @param limit 条数上限；<=0 或超容量时按容量夹取。
 * @return cJSON 数组（调用方 cJSON_Delete）；参数非法或分配失败返回 NULL。
 */
cJSON *tooljob_list(tooljob_t *jobs, int limit);

/** @brief 状态名（wire 值：running/done/failed/cancelled/timed_out）。 */
const char *tooljob_state(tool_job_state_t state);

/** @brief 快照 JSON（wire 形状）；调用方 cJSON_Delete。 */
cJSON *tooljob_json(const tool_job_info_t *info);

/** @brief 释放快照内 output/error。 */
void tooljob_rel(tool_job_info_t *info);

/** @brief 按策略夹取超时秒数（0 = 未指定，返回 0）。 */
int tooljob_clamp(int timeout_sec);

/** @brief 缺省作业目录（<data>/agentrt/jobs）。 */
const char *tooljob_root(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_TOOL_JOB_H */
