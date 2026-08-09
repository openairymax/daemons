# Scheduler Daemon — 任务调度守护进程

> **模块路径**: `agentrt/daemons/sched_d/` | **版本**: v0.1.0

## 概述

`daemons/sched_d/` 是 AgentRT 的任务调度守护进程，负责任务的分发、调度策略执行和任务状态管理。它根据配置的调度策略，将任务分发给最合适的 Agent 执行，支持轮询、加权、优先级和基于机器学习的调度策略，并提供 Agent 注册、状态更新和健康检查等管理功能。

### 核心职责

- **任务分发**：根据调度策略将任务分发给合适的 Agent
- **调度策略**：支持 4 种调度算法（轮询/加权/优先级/ML），可扩展的策略接口
- **任务队列**：管理全局任务队列，支持优先级和依赖
- **状态追踪**：跟踪任务的生命周期状态
- **Agent 管理**：Agent 注册、注销和状态更新
- **健康检查**：定期检查 Agent 的可用性和负载
- **重试机制**：失败任务自动重试，可配置重试策略

## 目录结构

```
sched_d/
├── CMakeLists.txt                    # 构建配置
├── README.md                         # 本文件
├── include/                          # 公共头文件
│   ├── scheduler_service.h           # 调度服务接口定义
│   ├── sched_svc_adapter.h           # 调度服务适配器接口
│   └── strategy_interface.h          # 调度策略接口定义
├── src/                              # 实现文件
│   ├── main.c                        # 守护进程入口
│   ├── sched_service_impl.c          # 调度服务核心实现
│   ├── sched_svc_adapter.c           # 请求解析与标准化适配器
│   ├── monitor.c                     # 调度监控
│   └── strategies/                   # 调度策略实现
│       ├── round_robin.c             # 轮询调度策略
│       ├── weighted.c                # 加权调度策略
│       ├── priority_based.c          # 优先级调度策略
│       └── ml_based.c                # 基于机器学习的调度策略
└── tests/                            # 单元测试
    ├── CMakeLists.txt
    ├── test_scheduler.c              # 调度器测试
    ├── test_strategies.c             # 策略测试
    └── test_dag.c                    # DAG 任务图引擎测试（串行/并行委派/group+consensus）
```

## DAG 任务图与并行派发（mac_framework 委派模式）

### DAG 引擎概览

`daemons/sched_d/` 内置 DAG 任务图执行引擎（工作大厅机制，详见伞仓 `docs/AirymaxRT/10-architecture/08-work-hall.md`）：

- **提交**：`sched_service_submit_dag(svc, dag_json, &dag_id)` 提交带依赖的任务图（JSON 声明 nodes: id/goal/depends），Kahn 拓扑校验拒绝环（`AIRY_ERR_CYCLE_DETECTED`）
- **执行**：dag 工作线程按拓扑派发就绪节点（依赖全部完成），节点经注入 executor（`sched_dispatch_executor` → agent_d spawn/invoke）真实执行
- **失败语义**：节点失败即中止该图并取消其余未完成节点（fail-fast 级联取消）
- **看板**：`sched_service_get_dag(svc, dag_id, &json)` 查询图/节点状态快照

### 并行度配置链路（完整透传）

DAG 并行派发通过 mac_framework（multi_agent_collaboration）**委派模式**接线：就绪节点批量收集 → 委派选人 → 线程池并发执行 → 汇聚回写。配置从顶层到引擎完整传递，**0 = 保持串行**（默认，兼容旧行为）：

```
┌──────────────────────────────┐
│ 服务框架配置（airy_svc_config_t）│
│  capabilities |= AIRY_SVC_CAP_BATCH      │ ← 声明 BATCH 能力
│  max_concurrent = N                      │ ← 并行度来源
└──────────────┬───────────────┘
               │ sched_svc_adapter.c: sched_config_from_common()
               │  （clamp 至 SCHED_DAG_MAX_NODES=64）
┌──────────────▼───────────────┐
│ sched_config_t                │
│  dag_max_parallel = N         │ ← 0 = 串行
│  dag_batch_size   = 0        │ ← 0 = 取 dag_max_parallel
└──────────────┬───────────────┘
               │ sched_service_create()
               │  （thread_pool min=max=dag_max_parallel 固定常驻）
┌──────────────▼───────────────┐
│ mac_framework (委派模式)       │
│  sched_mac_sync_agent()       │ ← performance/reliability/max_concurrent_tasks
│  delegate_batch → 线程池并发    │
│  complete_task → 槽位释放闭环   │
└──────────────────────────────┘
```

- **方式一（守护进程）**：环境变量 `AIRY_DAG_PARALLEL=N`（`0 < N ≤ SCHED_DAG_MAX_NODES`），见 `src/main.c`
- **方式二（框架接入）**：服务配置声明 `AIRY_SVC_CAP_BATCH` 且 `max_concurrent > 0`，适配器透传，见 `src/sched_svc_adapter.c`

> **注意**：`dag_max_parallel` 会创建同等数量的常驻线程池 worker（thread_pool 不动态扩增），透传时须 clamp 至 `SCHED_DAG_MAX_NODES` 防止失控。

### 测试用例（tests/test_dag.c）

| 用例 | 验证内容 |
|------|----------|
| `test_dag_parallel_delegation` | 菱形 DAG 委派模式并行：并发窗口实测 ≥2，拓扑顺序不破坏（用例 7） |
| `test_dag_group_consensus_collab` | 委派模式 + group 协作 + consensus 机制：3 名评审 agent 建组、批量委派、结果收集、MAJORITY 通过 / UNANIMOUS 一票否决、重复投票拒绝（用例 8） |

```bash
# 运行 DAG 相关测试
ctest --test-dir <build> -R 'sched_d_test_dag' --output-on-failure
```

## 核心组件说明

### 调度策略

sched_d 采用可扩展的策略接口（`strategy_interface_t`），每个策略实现统一的接口方法：

| 策略 | 文件 | 适用场景 | 说明 |
|------|------|----------|------|
| 轮询 | `round_robin.c` | 负载均衡 | 依次分配给可用 Agent，保证公平性 |
| 加权 | `weighted.c` | 异构 Agent 集群 | 按 Agent 权重分配，高权重 Agent 获得更多任务 |
| 优先级 | `priority_based.c` | 实时任务 | 优先处理高优先级任务，支持 4 级优先级 |
| ML | `ml_based.c` | 智能调度 | 基于机器学习模型预测最优 Agent，考虑历史表现 |

### 策略接口（strategy_interface.h）

所有调度策略必须实现以下接口：

```c
typedef struct {
    int (*create)(const sched_config_t *manager, void **data);
    int (*destroy)(void *data);
    int (*register_agent)(void *data, const agent_info_t *agent_info);
    int (*unregister_agent)(void *data, const char *agent_id);
    int (*update_agent_status)(void *data, const agent_info_t *agent_info);
    int (*schedule)(void *data, const task_info_t *task_info, sched_result_t **result);
    const char *(*get_name)();
    size_t (*get_available_agent_count)(void *data);
    size_t (*get_total_agent_count)(void *data);
} strategy_interface_t;
```

### 任务优先级

| 优先级 | 枚举值 | 说明 |
|--------|--------|------|
| LOW | `TASK_PRIORITY_LOW` | 低优先级，后台任务 |
| NORMAL | `TASK_PRIORITY_NORMAL` | 正常优先级，默认 |
| HIGH | `TASK_PRIORITY_HIGH` | 高优先级，实时任务 |
| URGENT | `TASK_PRIORITY_URGENT` | 紧急优先级，立即处理 |

### 调度监控（monitor）

调度监控模块，跟踪调度器的运行状态和性能指标：

- 调度成功率统计
- Agent 负载分布
- 任务等待时间
- 策略效果评估

## 接口说明

### 调度服务生命周期（scheduler_service.h）

```c
int sched_service_create(const sched_config_t *manager, sched_service_t **service);
int sched_service_destroy(sched_service_t *service);
int sched_service_reload_config(sched_service_t *service, const sched_config_t *manager);
```

### Agent 管理接口

```c
int sched_service_register_agent(sched_service_t *service, const agent_info_t *agent_info);
int sched_service_unregister_agent(sched_service_t *service, const char *agent_id);
int sched_service_update_agent_status(sched_service_t *service, const agent_info_t *agent_info);
```

### 任务调度接口

```c
int sched_service_schedule_task(sched_service_t *service, const task_info_t *task_info,
                                sched_result_t **result);
```

### 查询接口

```c
int sched_service_get_stats(sched_service_t *service, void **stats);
int sched_service_health_check(sched_service_t *service, bool *health_status);
```

### 核心数据结构

```c
typedef struct {
    sched_strategy_t strategy;
    uint32_t health_check_interval_ms;
    uint32_t stats_report_interval_ms;
    bool enable_ml_strategy;
    char *ml_model_path;
    uint32_t max_agents;
    uint32_t dag_max_parallel; /* DAG 节点并行度上限（0 = 串行，默认） */
    uint32_t dag_batch_size;   /* 每轮就绪节点批大小（0 = 取 dag_max_parallel） */
} sched_config_t;

typedef struct {
    char *task_id;
    char *task_description;
    task_priority_t priority;
    uint32_t timeout_ms;
    void *task_data;
    size_t task_data_size;
} task_info_t;

typedef struct {
    char *agent_id;
    char *agent_name;
    float load_factor;
    float success_rate;
    uint32_t avg_response_time_ms;
    bool is_available;
    float weight;
} agent_info_t;

typedef struct {
    char *selected_agent_id;
    float confidence;
    uint32_t estimated_time_ms;
} sched_result_t;
```

### JSON-RPC 2.0 方法

| 方法 | 说明 |
|------|------|
| `sched.submit` | 提交任务到队列 |
| `sched.dispatch` | 分发任务到 Agent |
| `sched.cancel` | 取消待执行任务 |
| `sched.status` | 查询任务执行状态 |
| `sched.queue.info` | 查询队列状态 |
| `sched.policy.set` | 设置调度策略 |
| `sched.agent.register` | 注册 Agent |
| `sched.agent.unregister` | 注销 Agent |
| `sched.agent.update` | 更新 Agent 状态 |

## 通信方式

| 方向 | 协议 | 说明 |
|------|------|------|
| 入站 | JSON-RPC 2.0 | 通过 IPC Service Bus 接收请求 |
| 出站 | JSON-RPC 2.0 | 向 Agent 发送任务执行指令 |

## 依赖关系

```
sched_d
├── common (svc_common, svc_logger, svc_config, ipc_service_bus,
│           method_dispatcher, jsonrpc_helpers, circuit_breaker)
└── monit_d  # 调度指标上报
```

## 构建说明

```bash
# 构建调度守护进程
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target sched_d

# 运行调度测试
ctest --test-dir build -R "test_scheduler|test_strategies" -V
```

## 使用示例

### 启动调度守护进程

```bash
# 启动调度守护进程
./sched_d --config sched_config.json

# 设置调度策略
./sched_d --policy weighted

# 指定工作线程数
./sched_d --workers 8
```

### 代码调用示例

```c
#include "daemons/sched_d/include/scheduler_service.h"

sched_config_t config = {
    .strategy = SCHED_STRATEGY_WEIGHTED,
    .health_check_interval_ms = 30000,
    .stats_report_interval_ms = 60000,
    .enable_ml_strategy = false,
    .max_agents = 64
};

sched_service_t *svc = NULL;
sched_service_create(&config, &svc);

agent_info_t agent = {
    .agent_id = "agent-001",
    .agent_name = "Code Review Agent",
    .load_factor = 0.3,
    .success_rate = 0.95,
    .avg_response_time_ms = 2500,
    .is_available = true,
    .weight = 1.5
};
sched_service_register_agent(svc, &agent);

task_info_t task = {
    .task_id = "task-123",
    .task_description = "Review PR #42",
    .priority = TASK_PRIORITY_NORMAL,
    .timeout_ms = 30000
};

sched_result_t *result = NULL;
sched_service_schedule_task(svc, &task, &result);

printf("Selected Agent: %s (confidence: %.2f)\n",
       result->selected_agent_id, result->confidence);

sched_service_destroy(svc);
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
