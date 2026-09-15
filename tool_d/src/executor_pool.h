/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor_pool.h
 * @brief R1-a (0.1.17): tool 执行面专用有界 worker 池。
 *
 * tool.execute 原先同步阻塞在 daemon RPC 共享处理池线程上：慢工具挤占
 * 共享池导致 approve/list/health_check 饿死；多个并发 interactive-approval
 * 等待可占满池使 approve 自身排队（批准死锁）。本池把执行隔离到独立
 * 有界线程集，调用方带预算等待：超时返回 AIRY_ERR_CANCELED 并合成取消
 * 结果，job 与池脱钩（detach）由 worker 完成后自清理，不影响其它在途
 * 任务。
 */

#ifndef TOOL_EXECUTOR_POOL_H
#define TOOL_EXECUTOR_POOL_H

#include "executor.h"
#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct executor_pool executor_pool_t;

/**
 * @brief Create a bounded tool-execution pool bound to an executor.
 * @param exec Executor (BORROW; must outlive the pool)
 * @return Pool handle, NULL on failure
 *
 * Worker count: env AIRY_TOOL_EXEC_WORKERS in [1,8], default 2.
 *
 * @ownership exec: BORROW; return: OWNER
 */
executor_pool_t *executor_pool_new(tool_executor_t *exec);

/**
 * @brief Destroy the pool (drains all in-flight jobs before returning).
 * @param pool Pool
 *
 * Queued jobs are resolved as AIRY_ERR_CANCELED; running jobs are awaited.
 * The executor is NOT destroyed here (owned by the service).
 *
 * @ownership pool: TRANSFER
 */
void executor_pool_free(executor_pool_t *pool);

/**
 * @brief Submit a tool execution and block for the result within a budget.
 * @param pool Pool
 * @param meta Tool metadata (BORROW; deep-copied into the job)
 * @param params_json Parameter JSON (BORROW; copied)
 * @param agent_id Caller agent ID (BORROW; copied, may be NULL)
 * @param out_result Output result (OWNER on return, caller AIRY_FREEs via
 *        tool_result_free)
 * @return 0 on success; AIRY_ERR_BUSY when the queue is full (backpressure
 *         fast-fail); AIRY_ERR_CANCELED when the wait budget expired (the
 *         job keeps running detached and its eventual real result is
 *         discarded); other error codes from tool_executor_run
 *
 * Wait budget: env AIRY_TOOL_WAIT_BUDGET_MS when set (>0), otherwise
 * max(meta, executor) timeout + slack (+ the interactive-approval ceiling
 * when interactive approval is enabled).
 *
 * @ownership pool: BORROW; meta/params_json/agent_id: BORROW
 */
int executor_pool_run(executor_pool_t *pool, const tool_metadata_t *meta,
                      const char *params_json, const char *agent_id,
                      tool_result_t **out_result);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_POOL_H */
