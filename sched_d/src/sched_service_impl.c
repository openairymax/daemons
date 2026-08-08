// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/**
 * @file sched_service_impl.c
 * @brief 调度服务核心实现
 * @details 定义 struct sched_service 并实现 scheduler_service.h 中的所有公共API
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "scheduler_service.h"
#include "svc_logger.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>
#include <airymax/sched.h>

/* ============================================================================
 * DAG 任务图内部结构（工作大厅机制：节点依赖 → 拓扑派发 → 状态看板）
 * ============================================================================ */

typedef struct sched_dag_node {
    char *id;                            /**< 节点 ID（计划节点 S_01 等） */
    char *goal;                          /**< 节点目标（executor 的 input） */
    char *role;                          /**< agent 角色（缺省 "coding"） */
    char *depends[SCHED_DAG_MAX_DEPS];   /**< 依赖节点 ID 数组 */
    size_t dep_count;
    sched_dag_node_status_t status;      /**< 节点状态 */
    char *output;                        /**< 执行输出（COMPLETED） */
    char *error;                         /**< 失败原因（FAILED） */
    uint64_t started_at_ms;
    uint64_t finished_at_ms;
} sched_dag_node_t;

typedef struct sched_dag {
    char *dag_id;                        /**< 图 ID */
    char *name;                          /**< 图名称（任务描述） */
    sched_dag_node_t *nodes[SCHED_DAG_MAX_NODES];
    size_t node_count;
    sched_dag_status_t status;           /**< 图整体状态 */
    size_t terminal_count;               /**< 已终态（completed/failed/canceled）节点数 */
    uint64_t created_at_ms;
    uint64_t finished_at_ms;
} sched_dag_t;

/* dag 工作线程（start_workers 中创建，实现在文件尾部 DAG 引擎段） */
static void *sched_dag_worker_thread(void *arg);

/* 队列/记录存储空间：任务记录数组（含已完成的查询历史）+ 环形 pending 队列 */
struct sched_service {
    sched_config_t config;
    agent_info_t *agents[AIRY_CAP_MAX_AGENTS];
    size_t agent_count;
    uint64_t total_tasks_scheduled;
    uint64_t total_success;
    int initialized;

    /* 任务队列（异步执行：入队 → 工作线程消费 → 选 agent → executor 派发） */
    task_record_t *tasks[AIRY_CAP_MAX_TASKS];
    size_t task_count;
    task_record_t *queue[AIRY_CAP_MAX_TASKS]; /* 环形队列，存 pending 任务 */
    size_t queue_head;
    size_t queue_tail;
    size_t task_seq; /* task_id 生成序号 */
    /* 统一锁：保护 agents 数组 + 任务队列 + 任务记录 + DAG 表；
     * 工作线程与 RPC 线程并发访问（原单线程事件驱动无需锁，引入
     * 工作线程后必须加锁，否则 register_agent 与选人读操作竞争） */
    airy_mtx_t lock;
    airy_cond_t queue_cond;
    sched_task_executor_t executor; /* 任务执行回调（daemon 注入，仅 worker 读） */
    volatile int worker_run;
    airy_thread_t worker_thread;

    /* ---- DAG 任务图执行引擎（工作大厅机制） ----
     * dags[] 为持久记录（与 tasks[] 一致：运行期间不释放，destroy 时清理）；
     * dag 工作线程扫描 active 图，将就绪节点逐个派发到 executor。 */
    sched_dag_t *dags[SCHED_DAG_MAX_DAGS];
    size_t dag_count;
    size_t dag_seq; /* dag_id 生成序号 */
    airy_cond_t dag_cond;
    volatile int dag_run;
    airy_thread_t dag_thread;
};

/* 当前毫秒时间戳（秒级 time(NULL) * 1000，够用于状态记录） */
static uint64_t sched_now_ms(void)
{
    return (uint64_t)time(NULL) * 1000ull;
}

int sched_service_create(const sched_config_t *config, sched_service_t **service)
{
    if (!config || !service) {
        SVC_LOG_ERROR("sched_service_create: NULL parameter (config=%p, service=%p)", (const void *)config, (const void *)service);
        return AIRY_ERR_INVALID_PARAM;
    }

    sched_service_t *svc = (sched_service_t *)AIRY_CALLOC(1, sizeof(sched_service_t));
    if (!svc) {
        SVC_LOG_ERROR("sched_service_create: calloc failed for service");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    __builtin_memcpy(&svc->config, config, sizeof(sched_config_t));
    if (config->ml_model_path)
        svc->config.ml_model_path = AIRY_STRDUP(config->ml_model_path);

    svc->initialized = 1;
    svc->queue_head = 0;
    svc->queue_tail = 0;
    svc->worker_run = 0;
    svc->worker_thread = AIRY_INVALID_THREAD;
    svc->executor = NULL;
    svc->dag_count = 0;
    svc->dag_seq = 0;
    svc->dag_run = 0;
    svc->dag_thread = AIRY_INVALID_THREAD;
    airy_mtx_init(&svc->lock);
    airy_cond_init(&svc->queue_cond);
    airy_cond_init(&svc->dag_cond);

    *service = svc;
    return 0;
}

int sched_service_destroy(sched_service_t *service)
{
    if (!service) {
        SVC_LOG_ERROR("sched_service_destroy: NULL service parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    /* 先停工作线程，防止其并发访问 agents/tasks（worker 会等待退出信号） */
    sched_service_stop_workers(service);

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]) {
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->agent_name);
            AIRY_FREE(service->agents[i]);
        }
    }
    for (size_t i = 0; i < service->task_count; i++) {
        task_record_t *rec = service->tasks[i];
        AIRY_FREE(rec->task_id);
        AIRY_FREE(rec->task_description);
        AIRY_FREE(rec->selected_agent_id);
        AIRY_FREE(rec->output);
        AIRY_FREE(rec->error);
        AIRY_FREE(rec);
    }
    service->task_count = 0;
    for (size_t i = 0; i < service->dag_count; i++) {
        sched_dag_t *dag = service->dags[i];
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *node = dag->nodes[j];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            for (size_t k = 0; k < node->dep_count; k++) {
                AIRY_FREE(node->depends[k]);
            }
            AIRY_FREE(node->output);
            AIRY_FREE(node->error);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->dag_id);
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
    }
    service->dag_count = 0;
    airy_mtx_unlock(&service->lock);

    AIRY_FREE((void *)service->config.ml_model_path);
    airy_cond_destroy(&service->dag_cond);
    airy_cond_destroy(&service->queue_cond);
    airy_mtx_destroy(&service->lock);
    AIRY_FREE(service);
    return 0;
}

int sched_service_register_agent(sched_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("sched_service_register_agent: NULL parameter or not initialized (service=%p, agent_info=%p, initialized=%d)", (const void *)service, (const void *)agent_info, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    if (service->agent_count >= AIRY_CAP_MAX_AGENTS) {
        SVC_LOG_ERROR("sched_service_register_agent: max agents exceeded (count=%zu, max=%d)", service->agent_count, AIRY_CAP_MAX_AGENTS);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OVERFLOW;
    }

    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            service->agents[i]->load_factor = agent_info->load_factor;
            service->agents[i]->success_rate = agent_info->success_rate;
            service->agents[i]->avg_response_time_ms = agent_info->avg_response_time_ms;
            service->agents[i]->is_available = agent_info->is_available;
            service->agents[i]->weight = agent_info->weight;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
    if (!new_agent) {
        SVC_LOG_ERROR("sched_service_register_agent: calloc failed for new agent");
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    if (!agent_info->agent_id) {
        SVC_LOG_ERROR("sched_service_register_agent: agent_info->agent_id is NULL");
        AIRY_FREE(new_agent);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_INVALID_PARAM;
    }
    new_agent->agent_id = AIRY_STRDUP(agent_info->agent_id);
    new_agent->agent_name =
        agent_info->agent_name ? AIRY_STRDUP(agent_info->agent_name) : AIRY_STRDUP("");
    if (!new_agent->agent_id || !new_agent->agent_name) {
        SVC_LOG_ERROR("sched_service_register_agent: strdup failed for agent fields (agent_id=%p, agent_name=%p)", (const void *)new_agent->agent_id, (const void *)new_agent->agent_name);
        AIRY_FREE(new_agent->agent_id);
        AIRY_FREE(new_agent->agent_name);
        AIRY_FREE(new_agent);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    new_agent->load_factor = agent_info->load_factor;
    new_agent->success_rate = agent_info->success_rate;
    new_agent->avg_response_time_ms = agent_info->avg_response_time_ms;
    new_agent->is_available = agent_info->is_available;
    new_agent->weight = agent_info->weight;

    service->agents[service->agent_count++] = new_agent;
    SVC_LOG_INFO("sched: agent registered: id=%s name=%s avail=%d weight=%.2f "
                 "success_rate=%.2f load=%.2f (total_agents=%zu)",
                 new_agent->agent_id, new_agent->agent_name, new_agent->is_available,
                 (double)new_agent->weight, (double)new_agent->success_rate,
                 (double)new_agent->load_factor, service->agent_count);
    airy_mtx_unlock(&service->lock);
    return 0;
}

int sched_service_unregister_agent(sched_service_t *service, const char *agent_id)
{
    if (!service || !agent_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_unregister_agent: NULL parameter or not initialized (service=%p, agent_id=%p, initialized=%d)", (const void *)service, (const void *)agent_id, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_id) == 0) {
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->agent_name);
            AIRY_FREE(service->agents[i]);

            for (size_t j = i; j < service->agent_count - 1; j++) {
                service->agents[j] = service->agents[j + 1];
            }
            service->agent_count--;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    SVC_LOG_ERROR("sched_service_unregister_agent: agent not found (agent_id=%s)", agent_id ? agent_id : "NULL");
    return AIRY_ERR_NOT_FOUND;
}

int sched_service_update_agent_status(sched_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("sched_service_update_agent_status: NULL parameter or not initialized (service=%p, agent_info=%p, initialized=%d)", (const void *)service, (const void *)agent_info, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            service->agents[i]->load_factor = agent_info->load_factor;
            service->agents[i]->success_rate = agent_info->success_rate;
            service->agents[i]->avg_response_time_ms = agent_info->avg_response_time_ms;
            service->agents[i]->is_available = agent_info->is_available;
            service->agents[i]->weight = agent_info->weight;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    SVC_LOG_ERROR("sched_service_update_agent_status: agent not found (agent_id=%s)", agent_info->agent_id ? agent_info->agent_id : "NULL");
    return AIRY_ERR_NOT_FOUND;
}

int sched_service_schedule_task(sched_service_t *service, const task_info_t *task_info,
                                sched_result_t **result)
{
    if (!service || !task_info || !result || !service->initialized) {
        SVC_LOG_ERROR("sched_service_schedule_task: NULL parameter or not initialized (service=%p, task_info=%p, result=%p, initialized=%d)", (const void *)service, (const void *)task_info, (const void *)result, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    /* 选人在锁内完成：工作线程与 register/unregister RPC 并发，必须互斥 */
    airy_mtx_lock(&service->lock);
    sched_result_t *res = (sched_result_t *)AIRY_CALLOC(1, sizeof(sched_result_t));
    if (!res) {
        SVC_LOG_ERROR("sched_service_schedule_task: calloc failed for result");
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    agent_info_t *best_agent = NULL;
    float best_score = -1.0f;

    for (size_t i = 0; i < service->agent_count; i++) {
        if (!service->agents[i]->is_available)
            continue;

        float score = 0.0f;
        switch (service->config.strategy) {
        case SCHED_STRATEGY_WEIGHTED:
            score = service->agents[i]->weight * service->agents[i]->success_rate *
                    (1.0f - service->agents[i]->load_factor);
            break;
        case SCHED_STRATEGY_ROUND_ROBIN:
            if (service->agent_count == 0) {
                score = 0.0f;
            } else {
                score = (float)(service->total_tasks_scheduled % service->agent_count);
                if ((size_t)score == i)
                    score = 100.0f;
                else
                    score = 0.0f;
            }
            break;
        case SCHED_STRATEGY_ML_BASED:
            score = service->agents[i]->success_rate * (1.0f - service->agents[i]->load_factor);
            break;
        case SCHED_STRATEGY_PRIORITY_BASED: {
            /* 优先级策略：在加权基础上按任务优先级放大选人分数
             * （语义对齐 strategies/priority_based.c 的 priority_weight：
             * 任务越紧急，越倾向选择健康度高、负载低的 agent）。 */
            float pw = 1.0f;
            if (task_info->priority >= TASK_PRIORITY_URGENT)
                pw = 4.0f;
            else if (task_info->priority >= TASK_PRIORITY_HIGH)
                pw = 2.0f;
            else if (task_info->priority >= TASK_PRIORITY_NORMAL)
                pw = 1.5f;
            score = (service->agents[i]->weight * service->agents[i]->success_rate *
                     (1.0f - service->agents[i]->load_factor)) * pw;
            break;
        }
        default:
            score = service->agents[i]->weight;
            break;
        }

        if (score > best_score) {
            best_score = score;
            best_agent = service->agents[i];
        }
    }

    service->total_tasks_scheduled++;

    if (best_agent) {
        res->selected_agent_id = AIRY_STRDUP(best_agent->agent_id);
        res->confidence = best_score > 0 ? (best_score > 1.0f ? 1.0f : best_score) : 0.5f;
        res->estimated_time_ms = best_agent->avg_response_time_ms;
        service->total_success++;
        SVC_LOG_DEBUG("sched: agent selected: task=%s agent=%s score=%.3f "
                      "confidence=%.2f strategy=%d candidates=%zu (avail filtered)",
                      task_info->task_id ? task_info->task_id : "?",
                      best_agent->agent_id, (double)best_score,
                      (double)res->confidence, (int)service->config.strategy,
                      service->agent_count);
    } else {
        SVC_LOG_ERROR("sched_service_schedule_task: no available agent found (agent_count=%zu, strategy=%d)", service->agent_count, service->config.strategy);
        res->selected_agent_id = NULL;
        res->confidence = 0.0f;
        res->estimated_time_ms = 0;
    }

    *result = res;
    airy_mtx_unlock(&service->lock);
    return 0;
}

int sched_service_get_stats(sched_service_t *service, void **stats)
{
    if (!service || !stats || !service->initialized) {
        SVC_LOG_ERROR("sched_service_get_stats: NULL parameter or not initialized (service=%p, stats=%p, initialized=%d)", (const void *)service, (const void *)stats, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *json_stats = (char *)AIRY_MALLOC(512);
    if (!json_stats) {
        SVC_LOG_ERROR("sched_service_get_stats: malloc failed for stats JSON");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    snprintf(json_stats, 512,
             "{\"agent_count\":%zu,\"total_tasks\":%llu,\"success_rate\":\"%.2f\",\"strategy\":%d}",
             service->agent_count, (unsigned long long)service->total_tasks_scheduled,
             service->total_tasks_scheduled > 0
                 ? (float)service->total_success / (float)service->total_tasks_scheduled
                 : 0.0f,
             service->config.strategy);

    *stats = json_stats;
    return 0;
}

int sched_service_health_check(sched_service_t *service, bool *health_status)
{
    if (!service || !health_status || !service->initialized) {
        SVC_LOG_ERROR("sched_service_health_check: NULL parameter or not initialized (service=%p, health_status=%p, initialized=%d)", (const void *)service, (const void *)health_status, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    bool all_healthy = true;
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]->load_factor > 0.95f) {
            all_healthy = false;
            break;
        }
    }

    *health_status = all_healthy && service->initialized;
    return 0;
}

int sched_service_reload_config(sched_service_t *service, const sched_config_t *config)
{
    if (!service || !config || !service->initialized) {
        SVC_LOG_ERROR("sched_service_reload_config: NULL parameter or not initialized (service=%p, config=%p, initialized=%d)", (const void *)service, (const void *)config, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_FREE((void *)service->config.ml_model_path);
    service->config.ml_model_path = NULL;

    __builtin_memcpy(&service->config, config, sizeof(sched_config_t));
    if (config->ml_model_path) {
        service->config.ml_model_path = AIRY_STRDUP(config->ml_model_path);
        if (!service->config.ml_model_path) {
            SVC_LOG_ERROR("sched_service_reload_config: strdup failed for ml_model_path (path=%s)", config->ml_model_path ? config->ml_model_path : "NULL");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
    }

    return 0;
}

/* ==================== 异步任务队列（断点 4） ==================== */

/*
 * 从环形队列选取并移除最高优先级任务（等优先级保持 FIFO，取最早入队者）。
 * 调用方必须持锁；队列为空返回 NULL。紧凑删除保证环形语义无空洞
 * （与 cancel_task 的队列重排一致）。
 */
static task_record_t *sched_queue_take_highest(sched_service_t *svc)
{
    if (svc->queue_head == svc->queue_tail)
        return NULL;

    /* 第一遍：找最高优先级（同优先级保留最先入队者 → FIFO 稳定） */
    size_t best = svc->queue_head;
    task_priority_t best_p = svc->queue[best]->priority;
    for (size_t qi = svc->queue_head; qi != svc->queue_tail;
         qi = (qi + 1) % AIRY_CAP_MAX_TASKS) {
        if (svc->queue[qi]->priority > best_p) {
            best = qi;
            best_p = svc->queue[qi]->priority;
        }
    }
    task_record_t *rec = svc->queue[best];

    /* 第二遍：紧凑删除 best 位置，把其后元素整体前移（环形） */
    size_t src = (best + 1) % AIRY_CAP_MAX_TASKS;
    size_t dst = best;
    while (src != svc->queue_tail) {
        svc->queue[dst] = svc->queue[src];
        src = (src + 1) % AIRY_CAP_MAX_TASKS;
        dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
    }
    svc->queue_tail = (svc->queue_tail + AIRY_CAP_MAX_TASKS - 1) %
                      AIRY_CAP_MAX_TASKS;
    size_t remain = (svc->queue_tail + AIRY_CAP_MAX_TASKS - svc->queue_head) %
                    AIRY_CAP_MAX_TASKS;
    SVC_LOG_DEBUG("sched: queue take: task=%s priority=%d wait_ms=%llu remain=%zu",
                  rec->task_id, (int)rec->priority,
                  (unsigned long long)(sched_now_ms() - rec->created_at_ms), remain);
    return rec;
}

/*
 * 工作线程：按优先级消费 pending 队列（最高优先级优先，同优先级 FIFO），
 * 依次执行 选 agent → executor（spawn+invoke）→ 回写状态。锁只在队列操作/
 * 状态回写时持有；选人与派发耗时操作在锁外，避免长任务阻塞 register_agent
 * 等 RPC。
 */
static void *sched_worker_thread(void *arg)
{
    sched_service_t *svc = (sched_service_t *)arg;

    while (1) {
        airy_mtx_lock(&svc->lock);
        /* 等待 pending 任务或退出信号 */
        while (svc->queue_head == svc->queue_tail && svc->worker_run) {
            SVC_LOG_DEBUG("sched: worker idle, waiting for tasks");
            airy_cond_wait(&svc->queue_cond, &svc->lock);
        }
        if (!svc->worker_run) {
            SVC_LOG_INFO("sched: worker received stop signal, exiting");
            airy_mtx_unlock(&svc->lock);
            break;
        }
        task_record_t *rec = sched_queue_take_highest(svc);
        if (!rec) { /* 理论不可达（非空队列必有可取任务），防御性跳过 */
            SVC_LOG_WARN("sched: worker woke but queue empty (spurious wakeup?)");
            airy_mtx_unlock(&svc->lock);
            continue;
        }
        rec->status = SCHED_TASK_STATUS_RUNNING;
        const uint64_t wait_ms = sched_now_ms() - rec->created_at_ms;
        SVC_LOG_INFO("sched: task dequeued: %s (priority=%d, wait_ms=%llu, "
                     "desc_len=%zu, timeout_ms=%u)",
                     rec->task_id, (int)rec->priority,
                     (unsigned long long)wait_ms,
                     rec->task_description ? strlen(rec->task_description) : 0,
                     rec->timeout_ms);
        airy_mtx_unlock(&svc->lock);

        /* ---- 锁外执行：选 agent + 派发（含 LLM 往返，耗时） ---- */
        char *selected = NULL;
        char *output = NULL;
        char *error = NULL;
        const uint64_t exec_t0 = sched_now_ms();

        task_info_t tinfo;
        __builtin_memset(&tinfo, 0, sizeof(tinfo));
        tinfo.task_id = rec->task_id;
        tinfo.task_description = rec->task_description;
        tinfo.priority = rec->priority;
        tinfo.timeout_ms = rec->timeout_ms;

        sched_result_t *sel = NULL;
        int sret = sched_service_schedule_task(svc, &tinfo, &sel);
        if (sret != AIRY_SUCCESS || !sel || !sel->selected_agent_id) {
            SVC_LOG_ERROR("sched: task %s select agent failed (rc=%d, sel=%p) — "
                          "check agent_d register_agent and agent availability",
                          rec->task_id, sret, (const void *)sel);
            error = AIRY_STRDUP("no available agent registered");
        } else {
            selected = AIRY_STRDUP(sel->selected_agent_id);
            if (!svc->executor) {
                sret = AIRY_ERR_SVC_NOT_READY;
                error = AIRY_STRDUP("task executor not injected");
                SVC_LOG_ERROR("sched: task %s executor not injected (set_executor "
                              "must be called before start_workers)", rec->task_id);
            } else {
                SVC_LOG_INFO("sched: dispatching task %s to agent %s",
                             rec->task_id, sel->selected_agent_id);
                sret = svc->executor(sel->selected_agent_id, rec->task_description, &output);
                if (sret != AIRY_SUCCESS || !output) {
                    SVC_LOG_ERROR("sched: executor returned failure for task %s "
                                  "(rc=%d, output=%p)", rec->task_id, sret,
                                  (const void *)output);
                    error = AIRY_STRDUP("agent dispatch failed");
                }
            }
        }
        if (sel) {
            AIRY_FREE(sel->selected_agent_id);
            AIRY_FREE(sel);
        }

        const uint64_t exec_elapsed_ms = sched_now_ms() - exec_t0;

        airy_mtx_lock(&svc->lock);
        rec->selected_agent_id = selected;
        selected = NULL;
        rec->finished_at_ms = sched_now_ms();
        if (sret == AIRY_SUCCESS && output) {
            if (rec->timeout_ms > 0 && exec_elapsed_ms > rec->timeout_ms) {
                /* 执行返回成功但超过任务级超时阈值：按超时失败上报，
                 * 保证 timeout_ms 语义闭环（无法中途打断同步 executor，
                 * 至少在结果层面生效）。 */
                rec->status = SCHED_TASK_STATUS_FAILED;
                rec->error = AIRY_STRDUP("task timed out");
                AIRY_FREE(output);
                output = NULL;
                SVC_LOG_WARN("Task timed out: %s (elapsed=%llu ms, limit=%u ms)",
                             rec->task_id, (unsigned long long)exec_elapsed_ms,
                             rec->timeout_ms);
            } else {
                rec->status = SCHED_TASK_STATUS_COMPLETED;
                rec->output = output;
                output = NULL;
                SVC_LOG_INFO("Task completed: %s (agent=%s, output_len=%zu)",
                             rec->task_id, rec->selected_agent_id ? rec->selected_agent_id : "?",
                             rec->output ? strlen(rec->output) : 0);
            }
        } else {
            rec->status = SCHED_TASK_STATUS_FAILED;
            rec->error = error;
            error = NULL;
            SVC_LOG_ERROR("Task failed: %s (agent=%s, error=%s)", rec->task_id,
                          rec->selected_agent_id ? rec->selected_agent_id : "?",
                          rec->error ? rec->error : "unknown");
        }
        airy_mtx_unlock(&svc->lock);

        AIRY_FREE(selected);
        AIRY_FREE(output);
        AIRY_FREE(error);
    }
    return NULL;
}

int sched_service_submit_task(sched_service_t *service, const task_info_t *task_info,
                              char **out_task_id)
{
    if (!service || !task_info || !out_task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_submit_task: NULL parameter or not initialized (service=%p, task_info=%p, out_task_id=%p, initialized=%d)", (const void *)service, (const void *)task_info, (const void *)out_task_id, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_task_id = NULL;

    /* task_id：客户端提供则采用；否则由服务端生成（时间戳+序号，保证唯一）。
     * 生成必须在锁内完成——RPC 经线程池并发处理（thread_pool_max=8），
     * task_seq++ 若在锁外并发递增会造成 task_id 重复（data race）。 */
    char id_buf[64];
    const int use_generated = !(task_info->task_id && task_info->task_id[0]);

    airy_mtx_lock(&service->lock);
    if (use_generated) {
        snprintf(id_buf, sizeof(id_buf), "task_%llu_%zu",
                 (unsigned long long)time(NULL), service->task_seq++);
    } else {
        snprintf(id_buf, sizeof(id_buf), "%s", task_info->task_id);
    }
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, id_buf) == 0) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_ERROR("sched_service_submit_task: duplicate task_id: %s", id_buf);
            return AIRY_ERR_ALREADY_EXISTS;
        }
    }
    size_t qlen = (service->queue_tail + AIRY_CAP_MAX_TASKS - service->queue_head) %
                  AIRY_CAP_MAX_TASKS;
    if (qlen >= AIRY_CAP_MAX_TASKS - 1 || service->task_count >= AIRY_CAP_MAX_TASKS) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: task queue full (task_count=%zu)", service->task_count);
        return AIRY_ERR_OVERFLOW;
    }

    task_record_t *rec = (task_record_t *)AIRY_CALLOC(1, sizeof(task_record_t));
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: calloc failed for task record");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->task_id = AIRY_STRDUP(id_buf);
    rec->task_description =
        AIRY_STRDUP(task_info->task_description ? task_info->task_description : "");
    if (!rec->task_id || !rec->task_description) {
        AIRY_FREE(rec->task_id);
        AIRY_FREE(rec->task_description);
        AIRY_FREE(rec);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->priority = task_info->priority;
    rec->timeout_ms = task_info->timeout_ms;
    rec->status = SCHED_TASK_STATUS_PENDING;
    rec->created_at_ms = sched_now_ms();

    service->tasks[service->task_count++] = rec;
    service->queue[service->queue_tail] = rec;
    service->queue_tail = (service->queue_tail + 1) % AIRY_CAP_MAX_TASKS;
    /* 唤醒工作线程（若未启动则任务保持 pending，启动后自然消费） */
    airy_cond_signal(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    *out_task_id = AIRY_STRDUP(id_buf);
    SVC_LOG_INFO("Task queued: %s (priority=%d, timeout_ms=%u, queue_depth=%zu, "
                 "desc_len=%zu, worker=%s)",
                 id_buf, (int)task_info->priority, task_info->timeout_ms, qlen + 1,
                 rec->task_description ? strlen(rec->task_description) : 0,
                 service->worker_run ? "running" : "NOT STARTED (task will stay pending)");
    return AIRY_SUCCESS;
}

int sched_service_get_task(sched_service_t *service, const char *task_id, char **out_json)
{
    if (!service || !task_id || !out_json || !service->initialized) {
        SVC_LOG_ERROR("sched_service_get_task: NULL parameter or not initialized (service=%p, task_id=%p, out_json=%p, initialized=%d)", (const void *)service, (const void *)task_id, (const void *)out_json, service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    task_record_t *rec = NULL;
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, task_id) == 0) {
            rec = service->tasks[i];
            break;
        }
    }
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_WARN("sched_service_get_task: task not found (task_id=%s)", task_id);
        return AIRY_ERR_NOT_FOUND;
    }

    static const char *status_names[] = {"pending", "running", "completed", "failed", "canceled"};
    const char *sname = (rec->status < SCHED_TASK_STATUS_COUNT) ? status_names[rec->status] : "unknown";

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "task_id", rec->task_id);
        cJSON_AddStringToObject(root, "status", sname);
        cJSON_AddNumberToObject(root, "priority", (double)rec->priority);
        cJSON_AddNumberToObject(root, "timeout_ms", (double)rec->timeout_ms);
        cJSON_AddStringToObject(root, "selected_agent_id",
                                rec->selected_agent_id ? rec->selected_agent_id : "");
        cJSON_AddStringToObject(root, "output", rec->output ? rec->output : "");
        cJSON_AddStringToObject(root, "error", rec->error ? rec->error : "");
        cJSON_AddNumberToObject(root, "created_at_ms", (double)rec->created_at_ms);
        cJSON_AddNumberToObject(root, "finished_at_ms", (double)rec->finished_at_ms);
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}

int sched_service_set_executor(sched_service_t *service, sched_task_executor_t executor)
{
    if (!service || !executor || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;
    /* 必须在 start_workers 之前注入：worker 启动后不再变更（仅 worker 读） */
    service->executor = executor;
    return AIRY_SUCCESS;
}

int sched_service_start_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;
    if (service->worker_thread != AIRY_INVALID_THREAD)
        return AIRY_SUCCESS; /* 已启动，幂等 */

    service->worker_run = 1;
    if (airy_thread_create(&service->worker_thread, sched_worker_thread, service) != 0) {
        service->worker_run = 0;
        service->worker_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler worker thread started");

    /* DAG 引擎工作线程（独立消费者：图节点拓扑派发） */
    service->dag_run = 1;
    if (airy_thread_create(&service->dag_thread, sched_dag_worker_thread, service) != 0) {
        service->dag_run = 0;
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: dag thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler DAG worker thread started");
    return AIRY_SUCCESS;
}

void sched_service_stop_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return;

    /* 先停 DAG 线程，再停任务队列线程（同一 dag_cond/lock 协调） */
    if (service->dag_thread != AIRY_INVALID_THREAD) {
        airy_mtx_lock(&service->lock);
        service->dag_run = 0;
        airy_cond_broadcast(&service->dag_cond);
        airy_mtx_unlock(&service->lock);
        airy_thread_join(service->dag_thread, NULL);
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_INFO("Scheduler DAG worker thread stopped");
    }

    if (service->worker_thread == AIRY_INVALID_THREAD)
        return;

    airy_mtx_lock(&service->lock);
    service->worker_run = 0;
    /* 唤醒可能阻塞在 cond_wait 的工作线程 */
    airy_cond_broadcast(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    airy_thread_join(service->worker_thread, NULL);
    service->worker_thread = AIRY_INVALID_THREAD;
    SVC_LOG_INFO("Scheduler worker thread stopped");
}

/* ============================================================================
 * DAG 任务图执行引擎实现（工作大厅机制）
 * ============================================================================ */

int sched_service_cancel_task(sched_service_t *service, const char *task_id)
{
    if (!service || !task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_cancel_task: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    int found = 0;
    for (size_t i = 0; i < service->task_count; i++) {
        task_record_t *rec = service->tasks[i];
        if (strcmp(rec->task_id, task_id) != 0)
            continue;
        found = 1;
        if (rec->status != SCHED_TASK_STATUS_PENDING) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_WARN("sched_service_cancel_task: task %s not cancelable (status=%d)",
                         task_id, (int)rec->status);
            return AIRY_ERR_BUSY;
        }
        /* 从环形队列移除（该任务在队列中位置不定，紧凑重排） */
        size_t qi = service->queue_head;
        size_t removed = 0;
        while (qi != service->queue_tail) {
            if (service->queue[qi] == rec) {
                service->queue[qi] = NULL;
                removed = 1;
                break;
            }
            qi = (qi + 1) % AIRY_CAP_MAX_TASKS;
        }
        if (removed) {
            /* 重排队列，去掉空洞（保持环形语义） */
            size_t src = (qi + 1) % AIRY_CAP_MAX_TASKS;
            size_t dst = qi;
            while (src != service->queue_tail) {
                service->queue[dst] = service->queue[src];
                src = (src + 1) % AIRY_CAP_MAX_TASKS;
                dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
            }
            service->queue_tail = (service->queue_tail + AIRY_CAP_MAX_TASKS - 1) %
                                  AIRY_CAP_MAX_TASKS;
        }
        rec->status = SCHED_TASK_STATUS_CANCELED;
        rec->finished_at_ms = sched_now_ms();
        rec->error = AIRY_STRDUP("canceled by user");
        SVC_LOG_INFO("Task canceled: %s", task_id);
        break;
    }
    airy_mtx_unlock(&service->lock);

    return found ? AIRY_SUCCESS : AIRY_ERR_NOT_FOUND;
}

/* 节点是否依赖已全部满足（均 COMPLETED） */
static int sched_dag_node_ready(const sched_dag_t *dag, size_t idx)
{
    const sched_dag_node_t *node = dag->nodes[idx];
    if (node->status != SCHED_DAG_NODE_PENDING)
        return 0;
    for (size_t k = 0; k < node->dep_count; k++) {
        const char *dep_id = node->depends[k];
        int dep_ok = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            if (strcmp(dag->nodes[j]->id, dep_id) == 0) {
                if (dag->nodes[j]->status == SCHED_DAG_NODE_COMPLETED) {
                    dep_ok = 1;
                } else if (dag->nodes[j]->status == SCHED_DAG_NODE_FAILED ||
                           dag->nodes[j]->status == SCHED_DAG_NODE_CANCELED) {
                    /* 依赖失败/取消 → 本节点级联取消 */
                    return -1;
                }
                break;
            }
        }
        if (!dep_ok) {
            /* 依赖未完成（或引用不存在的依赖：防御性视为未就绪） */
            return 0;
        }
    }
    return 1;
}

/* 扫描所有 active 图：返回第一个就绪节点所属的 dag 下标（无则 -1）。
 * 调用方持锁。 */
static long sched_dag_find_ready(sched_service_t *svc, sched_dag_node_t **out_node)
{
    for (size_t i = 0; i < svc->dag_count; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        for (size_t j = 0; j < dag->node_count; j++) {
            int r = sched_dag_node_ready(dag, j);
            if (r > 0) {
                *out_node = dag->nodes[j];
                return (long)i;
            }
        }
    }
    return -1;
}

/* 图是否全部节点进入终态 */
static int sched_dag_all_terminal(sched_service_t *svc, sched_dag_t *dag)
{
    for (size_t j = 0; j < dag->node_count; j++) {
        sched_dag_node_status_t st = dag->nodes[j]->status;
        if (st != SCHED_DAG_NODE_COMPLETED && st != SCHED_DAG_NODE_FAILED &&
            st != SCHED_DAG_NODE_CANCELED)
            return 0;
    }
    return 1;
}

static void *sched_dag_worker_thread(void *arg)
{
    sched_service_t *svc = (sched_service_t *)arg;

    while (1) {
        airy_mtx_lock(&svc->lock);
        while (sched_dag_find_ready(svc, &(sched_dag_node_t *){NULL}) < 0 && svc->dag_run) {
            SVC_LOG_DEBUG("sched: dag worker idle, waiting for ready nodes "
                          "(dags=%zu)", svc->dag_count);
            airy_cond_wait(&svc->dag_cond, &svc->lock);
        }
        if (!svc->dag_run) {
            SVC_LOG_INFO("sched: dag worker received stop signal, exiting");
            airy_mtx_unlock(&svc->lock);
            break;
        }

        sched_dag_node_t *node = NULL;
        long dag_idx = sched_dag_find_ready(svc, &node);
        if (dag_idx < 0 || !node) {
            /* 无就绪节点但未退出：可能全部图已完成，重新等待信号 */
            SVC_LOG_DEBUG("sched: dag worker woke but no ready node (all dags "
                          "done or deps unresolved)");
            airy_mtx_unlock(&svc->lock);
            continue;
        }
        sched_dag_t *dag = svc->dags[dag_idx];
        node->status = SCHED_DAG_NODE_RUNNING;
        node->started_at_ms = sched_now_ms();
        /* 快照 role/goal（节点持久存在，锁外可安全读） */
        const char *role = node->role ? node->role : "coding";
        const char *goal = node->goal ? node->goal : "";
        SVC_LOG_INFO("sched: DAG node dispatch: %s/%s role=%s deps=%zu "
                     "(wait since dag create=%llu ms, executor=%s)",
                     dag->dag_id, node->id, role, node->dep_count,
                     (unsigned long long)(sched_now_ms() - dag->created_at_ms),
                     svc->executor ? "ready" : "MISSING");
        airy_mtx_unlock(&svc->lock);

        /* ---- 锁外派发（executor 含 spawn/invoke/LLM 往返，耗时） ---- */
        char *output = NULL;
        int dret = svc->executor ? svc->executor(role, goal, &output)
                                 : AIRY_ERR_SVC_NOT_READY;

        airy_mtx_lock(&svc->lock);
        if (node->status == SCHED_DAG_NODE_CANCELED ||
            dag->status == SCHED_DAG_STATUS_CANCELED) {
            /* 节点/图在派发期间被取消：丢弃结果，节点保持取消语义。
             * cancel_dag 只将未完成节点置 canceled，RUNNING 节点完成后
             * 必须在此归一为 canceled（头文件承诺「不再上屏输出」）。 */
            if (output)
                AIRY_FREE(output);
            output = NULL; /* 尾部 cleanup 需避免 double-free */
            if (node->status != SCHED_DAG_NODE_CANCELED)
                node->status = SCHED_DAG_NODE_CANCELED;
        } else if (dret == AIRY_SUCCESS && output) {
            node->status = SCHED_DAG_NODE_COMPLETED;
            node->output = output;
            output = NULL;
            SVC_LOG_INFO("DAG node completed: %s/%s (role=%s, output_len=%zu)",
                         dag->dag_id, node->id, role, node->output ? strlen(node->output) : 0);
        } else {
            node->status = SCHED_DAG_NODE_FAILED;
            node->error = AIRY_STRDUP(dret == AIRY_ERR_SVC_NOT_READY
                                          ? "dag executor not injected"
                                          : "agent dispatch failed");
            SVC_LOG_ERROR("DAG node failed: %s/%s (role=%s, error=%s)", dag->dag_id, node->id,
                          role, node->error ? node->error : "unknown");
            /* 节点失败 → 图中止：取消其余未完成节点 */
            dag->status = SCHED_DAG_STATUS_FAILED;
            size_t cascade = 0;
            for (size_t j = 0; j < dag->node_count; j++) {
                sched_dag_node_t *n = dag->nodes[j];
                if (n->status == SCHED_DAG_NODE_PENDING || n->status == SCHED_DAG_NODE_READY) {
                    n->status = SCHED_DAG_NODE_CANCELED;
                    n->finished_at_ms = sched_now_ms();
                    cascade++;
                }
            }
            SVC_LOG_WARN("DAG aborted by node failure: %s (%zu downstream nodes "
                         "cascade-canceled)", dag->dag_id, cascade);
        }
        node->finished_at_ms = sched_now_ms();

        if (dag->status == SCHED_DAG_STATUS_ACTIVE && sched_dag_all_terminal(svc, dag)) {
            dag->status = SCHED_DAG_STATUS_COMPLETED;
            dag->finished_at_ms = sched_now_ms();
            SVC_LOG_INFO("DAG completed: %s (%zu nodes)", dag->dag_id, dag->node_count);
        }
        airy_mtx_unlock(&svc->lock);

        if (output)
            AIRY_FREE(output);
        /* 有节点完成 → 依赖它的节点可能就绪，唤醒 dag 线程 */
        airy_mtx_lock(&svc->lock);
        airy_cond_broadcast(&svc->dag_cond);
        airy_mtx_unlock(&svc->lock);
    }
    return NULL;
}

/* 解析 DAG JSON 并做 Kahn 拓扑校验（有环返回非 0），纯校验不写服务状态 */
static int sched_dag_validate_and_build(cJSON *root, sched_dag_t **out_dag)
{
    cJSON *nodes_json = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes_json) || cJSON_GetArraySize(nodes_json) == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }
    int n = cJSON_GetArraySize(nodes_json);
    if (n > SCHED_DAG_MAX_NODES) {
        return AIRY_ERR_OVERFLOW;
    }

    sched_dag_t *dag = (sched_dag_t *)AIRY_CALLOC(1, sizeof(sched_dag_t));
    if (!dag)
        return AIRY_ERR_OUT_OF_MEMORY;

    cJSON *name = cJSON_GetObjectItem(root, "name");
    dag->name = AIRY_STRDUP(cJSON_IsString(name) && name->valuestring ? name->valuestring
                                                                      : "unnamed_dag");
    dag->node_count = 0;

    /* 节点：id 必填且唯一；goal 缺省 ""；role 缺省 "coding"；depends 数组。
     * err 记录具体失败原因：结构非法 INVALID_PARAM / 分配失败 OOM /
     * 依赖环 CYCLE_DETECTED（此前所有失败一律归为环检测，误导调用方）。 */
    int err = AIRY_SUCCESS;
    for (int i = 0; i < n; i++) {
        cJSON *nj = cJSON_GetArrayItem(nodes_json, i);
        cJSON *nid = cJSON_GetObjectItem(nj, "id");
        if (!cJSON_IsString(nid) || !nid->valuestring || !nid->valuestring[0]) {
            err = AIRY_ERR_INVALID_PARAM;
            break;
        }
        for (size_t p = 0; p < dag->node_count; p++) {
            if (strcmp(dag->nodes[p]->id, nid->valuestring) == 0) {
                err = AIRY_ERR_INVALID_PARAM; /* 重复 id */
                break;
            }
        }
        if (err != AIRY_SUCCESS)
            break;

        sched_dag_node_t *node = (sched_dag_node_t *)AIRY_CALLOC(1, sizeof(sched_dag_node_t));
        if (!node) {
            err = AIRY_ERR_OUT_OF_MEMORY;
            break;
        }
        node->id = AIRY_STRDUP(nid->valuestring);
        cJSON *goal = cJSON_GetObjectItem(nj, "goal");
        node->goal = AIRY_STRDUP(cJSON_IsString(goal) && goal->valuestring ? goal->valuestring
                                                                           : "");
        cJSON *role = cJSON_GetObjectItem(nj, "role");
        node->role = AIRY_STRDUP(cJSON_IsString(role) && role->valuestring ? role->valuestring
                                                                           : "coding");
        if (!node->id || !node->goal || !node->role) {
            err = AIRY_ERR_OUT_OF_MEMORY;
        }
        cJSON *deps = cJSON_GetObjectItem(nj, "depends");
        if (cJSON_IsArray(deps) && err == AIRY_SUCCESS) {
            int dn = cJSON_GetArraySize(deps);
            if (dn > SCHED_DAG_MAX_DEPS) {
                err = AIRY_ERR_INVALID_PARAM;
            }
            for (int k = 0; k < dn && err == AIRY_SUCCESS; k++) {
                cJSON *dj = cJSON_GetArrayItem(deps, k);
                if (!cJSON_IsString(dj) || !dj->valuestring) {
                    err = AIRY_ERR_INVALID_PARAM;
                    break;
                }
                /* 依赖必须指向图内已存在节点（依赖声明在节点前或后均可，因此
                 * 第一遍先收集所有依赖，第二遍再做存在性校验） */
                node->depends[node->dep_count] = AIRY_STRDUP(dj->valuestring);
                if (!node->depends[node->dep_count]) {
                    err = AIRY_ERR_OUT_OF_MEMORY;
                    break;
                }
                node->dep_count++;
            }
        }
        dag->nodes[dag->node_count++] = node;
        if (err != AIRY_SUCCESS)
            break;
    }

    /* 依赖存在性校验（依赖 id 必须命中图内节点） */
    for (size_t i = 0; i < dag->node_count && err == AIRY_SUCCESS; i++) {
        sched_dag_node_t *node = dag->nodes[i];
        for (size_t k = 0; k < node->dep_count; k++) {
            int hit = 0;
            for (size_t j = 0; j < dag->node_count; j++) {
                if (strcmp(dag->nodes[j]->id, node->depends[k]) == 0) {
                    hit = 1;
                    break;
                }
            }
            if (!hit) {
                err = AIRY_ERR_INVALID_PARAM;
                break;
            }
        }
    }

    /* Kahn 拓扑校验：从入度 0 节点出发，统计可处理节点数，== n 则无环 */
    if (err == AIRY_SUCCESS) {
        size_t *indeg = (size_t *)AIRY_CALLOC(dag->node_count, sizeof(size_t));
        if (!indeg) {
            err = AIRY_ERR_OUT_OF_MEMORY;
        } else {
            for (size_t i = 0; i < dag->node_count; i++) {
                for (size_t k = 0; k < dag->nodes[i]->dep_count; k++) {
                    for (size_t j = 0; j < dag->node_count; j++) {
                        if (strcmp(dag->nodes[j]->id, dag->nodes[i]->depends[k]) == 0) {
                            indeg[i]++;
                            break;
                        }
                    }
                }
            }
            size_t processed = 0;
            int progressed = 1;
            while (progressed) {
                progressed = 0;
                for (size_t i = 0; i < dag->node_count; i++) {
                    if (indeg[i] == SIZE_MAX)
                        continue; /* 已处理 */
                    int all_ok = 1;
                    for (size_t k = 0; k < dag->nodes[i]->dep_count && all_ok; k++) {
                        for (size_t j = 0; j < dag->node_count; j++) {
                            if (strcmp(dag->nodes[j]->id, dag->nodes[i]->depends[k]) == 0) {
                                if (indeg[j] != SIZE_MAX)
                                    all_ok = 0; /* 依赖尚未处理 */
                                break;
                            }
                        }
                    }
                    if (all_ok) {
                        indeg[i] = SIZE_MAX;
                        processed++;
                        progressed = 1;
                    }
                }
            }
            AIRY_FREE(indeg);
            if (processed != dag->node_count)
                err = AIRY_ERR_CYCLE_DETECTED; /* 存在环 */
        }
    }

    if (err != AIRY_SUCCESS) {
        /* 释放半成品 */
        for (size_t i = 0; i < dag->node_count; i++) {
            sched_dag_node_t *node = dag->nodes[i];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            for (size_t k = 0; k < node->dep_count; k++)
                AIRY_FREE(node->depends[k]);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->dag_id);
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        return err;
    }

    *out_dag = dag;
    return AIRY_SUCCESS;
}

int sched_service_submit_dag(sched_service_t *service, const char *dag_json, char **out_dag_id)
{
    if (!service || !dag_json || !out_dag_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_submit_dag: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_dag_id = NULL;

    cJSON *root = cJSON_Parse(dag_json);
    if (!root) {
        SVC_LOG_ERROR("sched_service_submit_dag: invalid DAG JSON");
        return AIRY_ERR_PARSE_ERROR;
    }

    sched_dag_t *dag = NULL;
    int vret = sched_dag_validate_and_build(root, &dag);
    if (vret != AIRY_SUCCESS || !dag) {
        cJSON_Delete(root);
        SVC_LOG_ERROR("sched_service_submit_dag: validation failed (rc=%d)", vret);
        /* 错误码如实透传（环 INVALID_PARAM/OVERFLOW/OOM 各有语义，不再归一） */
        return vret;
    }

    airy_mtx_lock(&service->lock);
    if (service->dag_count >= SCHED_DAG_MAX_DAGS) {
        airy_mtx_unlock(&service->lock);
        /* 释放构建好的图（root 在出口统一释放） */
        for (size_t i = 0; i < dag->node_count; i++) {
            sched_dag_node_t *node = dag->nodes[i];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            for (size_t k = 0; k < node->dep_count; k++)
                AIRY_FREE(node->depends[k]);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        cJSON_Delete(root);
        return AIRY_ERR_OVERFLOW;
    }

    char id_buf[64];
    snprintf(id_buf, sizeof(id_buf), "dag_%llu_%zu", (unsigned long long)time(NULL),
             service->dag_seq++);
    dag->dag_id = AIRY_STRDUP(id_buf);
    if (!dag->dag_id) {
        airy_mtx_unlock(&service->lock);
        for (size_t i = 0; i < dag->node_count; i++) {
            sched_dag_node_t *node = dag->nodes[i];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            for (size_t k = 0; k < node->dep_count; k++)
                AIRY_FREE(node->depends[k]);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        cJSON_Delete(root);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    dag->status = SCHED_DAG_STATUS_ACTIVE;
    dag->created_at_ms = sched_now_ms();
    service->dags[service->dag_count++] = dag;

    /* 唤醒 dag 工作线程（入口节点可能已就绪） */
    airy_cond_broadcast(&service->dag_cond);
    airy_mtx_unlock(&service->lock);

    *out_dag_id = AIRY_STRDUP(id_buf);
    cJSON_Delete(root);
    SVC_LOG_INFO("DAG submitted: %s (%zu nodes)", id_buf, dag->node_count);
    return AIRY_SUCCESS;
}

int sched_service_get_dag(sched_service_t *service, const char *dag_id, char **out_json)
{
    if (!service || !dag_id || !out_json || !service->initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    sched_dag_t *dag = NULL;
    for (size_t i = 0; i < service->dag_count; i++) {
        if (strcmp(service->dags[i]->dag_id, dag_id) == 0) {
            dag = service->dags[i];
            break;
        }
    }
    if (!dag) {
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    static const char *dag_status_names[] = {"active", "completed", "failed", "canceled"};
    static const char *node_status_names[] = {"pending", "ready", "running", "completed",
                                              "failed", "canceled"};

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "dag_id", dag->dag_id);
        cJSON_AddStringToObject(root, "name", dag->name ? dag->name : "");
        cJSON_AddStringToObject(root, "status",
                                dag_status_names[dag->status % SCHED_DAG_STATUS_COUNT]);
        cJSON_AddNumberToObject(root, "node_count", (double)dag->node_count);
        size_t done = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_status_t st = dag->nodes[j]->status;
            if (st == SCHED_DAG_NODE_COMPLETED || st == SCHED_DAG_NODE_FAILED ||
                st == SCHED_DAG_NODE_CANCELED)
                done++;
        }
        cJSON_AddNumberToObject(root, "progress", (double)done);
        cJSON_AddNumberToObject(root, "created_at_ms", (double)dag->created_at_ms);
        cJSON_AddNumberToObject(root, "finished_at_ms", (double)dag->finished_at_ms);

        cJSON *nodes = cJSON_CreateArray();
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *node = dag->nodes[j];
            cJSON *nj = cJSON_CreateObject();
            cJSON_AddStringToObject(nj, "id", node->id);
            cJSON_AddStringToObject(nj, "goal", node->goal ? node->goal : "");
            cJSON_AddStringToObject(nj, "role", node->role ? node->role : "");
            cJSON_AddStringToObject(
                nj, "status",
                node_status_names[node->status % SCHED_DAG_NODE_COUNT]);
            cJSON *deps = cJSON_CreateArray();
            for (size_t k = 0; k < node->dep_count; k++)
                cJSON_AddItemToArray(deps, cJSON_CreateString(node->depends[k]));
            cJSON_AddItemToObject(nj, "depends", deps);
            if (node->output)
                cJSON_AddStringToObject(nj, "output", node->output);
            if (node->error)
                cJSON_AddStringToObject(nj, "error", node->error);
            cJSON_AddNumberToObject(nj, "started_at_ms", (double)node->started_at_ms);
            cJSON_AddNumberToObject(nj, "finished_at_ms", (double)node->finished_at_ms);
            cJSON_AddItemToArray(nodes, nj);
        }
        cJSON_AddItemToObject(root, "nodes", nodes);
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}

int sched_service_cancel_dag(sched_service_t *service, const char *dag_id)
{
    if (!service || !dag_id || !service->initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    sched_dag_t *dag = NULL;
    for (size_t i = 0; i < service->dag_count; i++) {
        if (strcmp(service->dags[i]->dag_id, dag_id) == 0) {
            dag = service->dags[i];
            break;
        }
    }
    if (!dag) {
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    if (dag->status != SCHED_DAG_STATUS_ACTIVE) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_WARN("sched_service_cancel_dag: dag %s not active (status=%d)", dag_id,
                     (int)dag->status);
        return AIRY_ERR_BUSY;
    }

    dag->status = SCHED_DAG_STATUS_CANCELED;
    dag->finished_at_ms = sched_now_ms();
    for (size_t j = 0; j < dag->node_count; j++) {
        sched_dag_node_t *node = dag->nodes[j];
        if (node->status == SCHED_DAG_NODE_PENDING || node->status == SCHED_DAG_NODE_READY) {
            node->status = SCHED_DAG_NODE_CANCELED;
            node->finished_at_ms = sched_now_ms();
            node->error = AIRY_STRDUP("canceled by user");
        }
        /* RUNNING 节点：保留运行，完成后 worker 发现已取消即丢弃输出 */
    }
    airy_cond_broadcast(&service->dag_cond);
    airy_mtx_unlock(&service->lock);

    SVC_LOG_INFO("DAG canceled: %s", dag_id);
    return AIRY_SUCCESS;
}

int sched_service_checkpoint_save(sched_service_t *service, char **out_json)
{
    if (!service || !out_json || !service->initialized) {
        SVC_LOG_ERROR("sched_service_checkpoint_save: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    /* 快照在锁内统计（队列/DAG 状态与 register/submit 并发，需一致视图） */
    airy_mtx_lock(&service->lock);
    size_t pending = 0, running = 0;
    size_t qi = service->queue_head;
    while (qi != service->queue_tail) {
        task_status_t st = service->queue[qi]->status;
        if (st == SCHED_TASK_STATUS_PENDING)
            pending++;
        else if (st == SCHED_TASK_STATUS_RUNNING)
            running++;
        qi = (qi + 1) % AIRY_CAP_MAX_TASKS;
    }
    size_t active_dags = 0, completed_dags = 0;
    size_t dag_total = service->dag_count;
    for (size_t i = 0; i < dag_total; i++) {
        sched_dag_status_t ds = service->dags[i]->status;
        if (ds == SCHED_DAG_STATUS_ACTIVE)
            active_dags++;
        else if (ds == SCHED_DAG_STATUS_COMPLETED)
            completed_dags++;
    }

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddNumberToObject(root, "agent_count", (double)service->agent_count);
        cJSON_AddNumberToObject(root, "total_tasks",
                                (double)service->total_tasks_scheduled);
        cJSON_AddNumberToObject(root, "pending", (double)pending);
        cJSON_AddNumberToObject(root, "running", (double)running);
        cJSON_AddNumberToObject(root, "dag_count", (double)service->dag_count);
        cJSON_AddNumberToObject(root, "active_dags", (double)active_dags);
        cJSON_AddNumberToObject(root, "completed_dags", (double)completed_dags);
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)sched_now_ms());
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    SVC_LOG_INFO("Checkpoint saved (pending=%zu, running=%zu, dags=%zu)", pending, running,
                 dag_total);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}
