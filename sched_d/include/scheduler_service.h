/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file scheduler_service.h
 * @brief 调度服务接口定义
 * @details 负责任务调度，选择最合适的 Agent
 */

#ifndef AIRY_RT_SCHEDULER_SERVICE_H
#define AIRY_RT_SCHEDULER_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 调度策略类型
 */
typedef enum {
    SCHED_STRATEGY_ROUND_ROBIN,
    SCHED_STRATEGY_WEIGHTED,
    SCHED_STRATEGY_ML_BASED,
    SCHED_STRATEGY_PRIORITY_BASED,
    SCHED_STRATEGY_COUNT
} sched_strategy_t;

/**
 * @brief 任务优先级
 */
typedef enum {
    TASK_PRIORITY_LOW,
    TASK_PRIORITY_NORMAL,
    TASK_PRIORITY_HIGH,
    TASK_PRIORITY_URGENT,
    TASK_PRIORITY_COUNT
} task_priority_t;

/**
 * @brief 任务信息
 */
typedef struct {
    char *task_id;
    char *task_description;
    task_priority_t priority;
    uint32_t timeout_ms;
    void *task_data;
    size_t task_data_size;
} task_info_t;

/**
 * @brief 任务生命周期状态（异步队列：入队→选中→执行→完成/失败）
 * @note 命名用 SCHED_ 前缀：types.h 已占用 TASK_STATUS_* 宏，避免枚举名展开冲突
 */
typedef enum {
    SCHED_TASK_STATUS_PENDING,
    SCHED_TASK_STATUS_RUNNING,
    SCHED_TASK_STATUS_COMPLETED,
    SCHED_TASK_STATUS_FAILED,
    SCHED_TASK_STATUS_CANCELED,
    SCHED_TASK_STATUS_COUNT
} task_status_t;

/**
 * @brief 任务记录（队列条目，get_task 查询依据）
 */
typedef struct {
    char *task_id;
    char *task_description;
    task_priority_t priority;
    uint32_t timeout_ms;
    task_status_t status;
    char *selected_agent_id;
    char *output;
    char *error;
    uint64_t created_at_ms;
    uint64_t finished_at_ms;
} task_record_t;

/**
 * @brief 任务执行回调（由 daemon 注入：选 agent + spawn + invoke）
 * @param agent_id 选中的 agent（role）
 * @param task_description 任务描述（作为 invoke 的 input）
 * @param out_output 执行输出（AIRY_MALLOC，调用方释放）
 * @return 0 成功，非 0 失败
 */
typedef int (*sched_task_executor_t)(const char *agent_id, const char *task_description,
                                     char **out_output);


#define AIRY_CAP_MAX_TASKS 256

/**
 * @brief Agent 信息
 */
typedef struct {
    char *agent_id; /**< Agent ID */
    char *agent_name;
    float load_factor;
    float success_rate;
    uint32_t avg_response_time_ms;
    bool is_available;
    float weight;
} agent_info_t;

/**
 * @brief 调度结果
 */
typedef struct {
    char *selected_agent_id;
    float confidence;
    uint32_t estimated_time_ms;
} sched_result_t;

/**
 * @brief 调度服务配置
 */
typedef struct {
    sched_strategy_t strategy;
    uint32_t health_check_interval_ms;
    uint32_t stats_report_interval_ms;
    bool enable_ml_strategy;
    char *ml_model_path;
    uint32_t max_agents;
    /* ---- DAG 并行派发（mac_framework 委派模式接线，0 = 保持串行） ----
     * dag_max_parallel: 单轮同时派发的就绪节点上限（≤ SCHED_DAG_MAX_NODES）。
     *                    0 = 保持现有单节点串行派发（兼容旧行为）。
     * dag_batch_size:   每轮就绪节点批大小（≤ dag_max_parallel，0 = 默认取
     *                    dag_max_parallel）。批量收集后经 mac_framework 委派
     *                    → 线程池并发执行 → 汇聚回写节点状态。 */
    uint32_t dag_max_parallel;
    uint32_t dag_batch_size;
    /* ---- 失败分级语义（改进3：Codex Fatal/普通 三态收敛到 sched 层） ----
     * dag_fatal_cascade: true（生产默认）→ 仅 FATAL 类失败级联取消整个图
     * （fail-closed），普通失败仅标记节点 FAILED 并取消依赖它的不可达下游，
     * 图其余独立分支继续执行；false → 任意节点失败即级联取消整图（旧行为）。 */
    bool dag_fatal_cascade;
} sched_config_t;

/**
 * @brief 调度服务句柄
 */
typedef struct sched_service sched_service_t;

/**
 * @brief 创建调度服务
 * @param manager 配置信息
 * @param service 输出参数，返回创建的服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_create(const sched_config_t *manager, sched_service_t **service);

/**
 * @brief 销毁调度服务
 * @param service 服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_destroy(sched_service_t *service);

/**
 * @brief 注册 Agent
 * @param service 服务句柄
 * @param agent_info Agent 信息
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_register_agent(sched_service_t *service, const agent_info_t *agent_info);

/**
 * @brief 注销 Agent
 * @param service 服务句柄
 * @param agent_id Agent ID
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_unregister_agent(sched_service_t *service, const char *agent_id);

/**
 * @brief 更新 Agent 状态
 * @param service 服务句柄
 * @param agent_info Agent 信息
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_update_agent_status(sched_service_t *service, const agent_info_t *agent_info);

/**
 * @brief 调度任务
 * @param service 服务句柄
 * @param task_info 任务信息
 * @param result 输出参数，返回调度结果
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_schedule_task(sched_service_t *service, const task_info_t *task_info,
                                sched_result_t **result);

/**
 * @brief 获取调度统计信息
 * @param service 服务句柄
 * @param stats 输出参数，返回统计信息
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_get_stats(sched_service_t *service, void **stats);

/**
 * @brief 健康检查
 * @param service 服务句柄
 * @param health_status 输出参数，返回健康状态
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_health_check(sched_service_t *service, bool *health_status);

/**
 * @brief 重载配置
 * @param service 服务句柄
 * @param manager 新的配置信息
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_reload_config(sched_service_t *service, const sched_config_t *manager);

/**
 * @brief 提交任务（异步队列）：入队后立即返回，工作线程随后执行
 * @param service 服务句柄
 * @param task_info 任务信息（task_id 为 NULL 时由服务端生成）
 * @param out_task_id 输出参数，返回实际生效的任务 ID（AIRY_MALLOC，调用方 AIRY_FREE）
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_submit_task(sched_service_t *service, const task_info_t *task_info,
                              char **out_task_id);

/**
 * @brief 查询任务状态
 * @param service 服务句柄
 * @param task_id 任务 ID
 * @param out_json 输出参数，返回任务状态 JSON（AIRY_MALLOC，调用方 AIRY_FREE）
 * @return 0 表示成功；AIRY_ERR_NOT_FOUND 表示任务不存在
 */
int sched_service_get_task(sched_service_t *service, const char *task_id, char **out_json);

/**
 * @brief 注入任务执行回调（必须在 start_workers 之前调用）
 * @param service 服务句柄
 * @param executor 执行回调（选中 agent 后由工作线程调用）
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_set_executor(sched_service_t *service, sched_task_executor_t executor);

/**
 * @brief 启动任务队列工作线程（消费 pending 队列并执行）
 * @param service 服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int sched_service_start_workers(sched_service_t *service);

/**
 * @brief 停止任务队列工作线程（destroy 前调用；幂等）
 * @param service 服务句柄
 */
void sched_service_stop_workers(sched_service_t *service);

/**
 * @brief 取消任务（仅 PENDING 可取消；RUNNING/终态返回 AIRY_ERR_BUSY）
 * @param service 服务句柄
 * @param task_id 任务 ID
 * @return 0 成功；AIRY_ERR_NOT_FOUND 任务不存在；AIRY_ERR_BUSY 不可取消
 */
int sched_service_cancel_task(sched_service_t *service, const char *task_id);

/* ============================================================================
 * DAG 任务图执行引擎（工作大厅机制，见 08-work-hall.md）
 *
 * 提交一个带依赖的任务图：dag 工作线程按拓扑顺序派发就绪节点（依赖全部
 * 完成后），每个节点经注入的 executor（sched_dispatch_executor → agent_d
 * spawn/invoke）真实执行；节点失败即中止该图并取消其余未完成节点。
 * 数据来源：think_d 的 GCCP+GRAD 计划（nodes: id/goal/depends[role]）。
 * ============================================================================ */


typedef enum {
    SCHED_DAG_NODE_PENDING = 0,
    SCHED_DAG_NODE_READY,
    SCHED_DAG_NODE_RUNNING,
    SCHED_DAG_NODE_COMPLETED,
    SCHED_DAG_NODE_FAILED,
    SCHED_DAG_NODE_CANCELED,
    SCHED_DAG_NODE_COUNT
} sched_dag_node_status_t;


typedef enum {
    SCHED_DAG_STATUS_ACTIVE = 0,
    SCHED_DAG_STATUS_COMPLETED,
    SCHED_DAG_STATUS_FAILED,
    SCHED_DAG_STATUS_CANCELED,
    SCHED_DAG_STATUS_COUNT
} sched_dag_status_t;


#define SCHED_DAG_MAX_NODES 64
#define SCHED_DAG_MAX_DEPS 8
#define SCHED_DAG_MAX_DAGS 32

/**
 * @brief 提交 DAG 任务图（异步：入图后 dag 工作线程按拓扑执行）
 * @param service 服务句柄
 * @param dag_json 图描述 JSON 字符串：
 *   {"name":"...","nodes":[{"id":"S_01","goal":"...","role":"coding",
 *    "depends":["S_02",...]}, ...]}
 *   - role 缺省 "coding"；depends 缺省空（入口节点）
 *   - 图必须无环（Kahn 拓扑校验，有环返回 AIRY_ERR_CYCLE_DETECTED）
 * @param out_dag_id 输出参数，返回 dag_id（AIRY_MALLOC，调用方 AIRY_FREE）
 * @return 0 成功；AIRY_ERR_INVALID_PARAM 非法 JSON/空节点/超上限；
 *         AIRY_ERR_CYCLE_DETECTED 存在依赖环
 */
int sched_service_submit_dag(sched_service_t *service, const char *dag_json, char **out_dag_id);

/**
 * @brief 查询 DAG 状态（看板快照：图状态/节点状态/进度/输出）
 * @param service 服务句柄
 * @param dag_id DAG ID
 * @param out_json 输出参数，返回 JSON（AIRY_MALLOC，调用方 AIRY_FREE）
 * @return 0 成功；AIRY_ERR_NOT_FOUND 不存在
 */
int sched_service_get_dag(sched_service_t *service, const char *dag_id, char **out_json);

/**
 * @brief 取消 DAG（未完成节点全部置 canceled；RUNNING 节点完成后不再上屏输出）
 * @param service 服务句柄
 * @param dag_id DAG ID
 * @return 0 成功；AIRY_ERR_NOT_FOUND 不存在
 */
int sched_service_cancel_dag(sched_service_t *service, const char *dag_id);

/**
 * @brief 保存调度检查点（L2 协议标准方法 sched.checkpoint_save）
 * @param service 服务句柄
 * @param out_json 输出参数，返回当前队列/DAG 状态快照 JSON（AIRY_MALLOC，调用方 AIRY_FREE）：
 *   {"agent_count":N,"total_tasks":T,"pending":P,"running":R,
 *    "dag_count":D,"active_dags":A,"completed_dags":C,"timestamp_ms":...}
 * @return 0 成功；AIRY_ERR_INVALID_PARAM 参数非法
 * @note 快照为内存视图（不落盘），由上层（如 monit_d / 集群管理器）决定持久化；
 *       供调度状态观测与故障恢复前的状态导出。
 */
int sched_service_checkpoint_save(sched_service_t *service, char **out_json);

#endif /* AIRY_RT_SCHEDULER_SERVICE_H */
