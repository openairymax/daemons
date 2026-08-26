# Scheduler Daemon — 任务调度守护进程

> **模块路径**: `agentrt/daemons/sched_d/`

## 定位

`sched_d` 是 AgentRT 的任务调度守护进程：维护 Agent 注册表，按调度策略为任务选择 Agent，
异步执行（真实派发：经 `agent.sock` 调用 agent_d `spawn` + `invoke` + `terminate`），并支持
DAG 工作流（提交/状态/取消）与蓝图调度 `roadmap.*` 方法族（L1 状态机 + L2 语义缓存 + L3
全量规划三级路由）。对外暴露 `sched.*` JSON-RPC 方法族（L2 服务协议）。

## 架构

```
gateway_d ──(sched.* JSON-RPC)──▶ sched_d
                                    │ sched_service（任务队列 + worker 线程）
                                    │   ├─ 策略：round_robin / weighted /
                                    │   │         priority_based / ml_based
                                    │   ├─ DAG 调度（dag_max_parallel 并行或串行）
                                    │   └─ 执行器回调 sched_dispatch_executor
                                    ▼
                             agent.sock（真实派发）
                             agent_d: spawn → invoke → terminate
```

- `schedule_task` 异步入队：立即返回 `task_id` + `status=pending`，worker 线程随后完成
  选 Agent → spawn → invoke → 状态写回（`get_task` 查询）；空任务（task_id 与
  task_description 均缺）拒绝入队。
- 真实派发（P2.2）：`sched_dispatch_task` 经 `agent.sock`（`AIRY_SCHED_AGENT_SOCK`
  覆盖，默认 `$AIRY_RUNTIME_DIR/agent.sock`）调用 agent_d `spawn`（role+language=python）
  → `invoke`（agent_id+input+workspace_dir）→ `terminate`；失败如实上报，绝不用伪造数据
  替代真实执行。可用 `AIRY_SCHED_DISPATCH=0` 关闭（任务不执行）。
- DAG 失败语义（`dag_fatal_cascade`）：生产默认仅 **FATAL** 级联取消整个图，普通失败
  不中断独立分支；`AIRY_DAG_FATAL_CASCADE=0` 恢复传统「任一节点失败即中止」。
- 蓝图调度（roadmap_rpc）：`airy_roadmap_sched_*` 三级路由（L1 状态机命中 / L2 语义缓存
  命中 / L3 全量规划），L2 语义缓存独立持久化到
  `$AIRY_DATA_DIR/agentrt/roadmap/l2_semantic_cache.json`（与 CLI 共用，跨进程共享）。

## JSON-RPC 接口表

监听端点：Unix socket `$AIRY_RUNTIME_DIR/sched.sock`（`--tcp` 或 Windows 下为
TCP `127.0.0.1:8083`，Windows pipe `\\.\pipe\airy_sched`）。以下方法由
`method_dispatcher_register` 实际注册（main.c + roadmap_rpc.c），共 18 个：

| 方法 | 参数 | 返回要点 | 说明 |
|------|------|----------|------|
| `register_agent` | `agent`（对象：`agent_id` 必填、`agent_name`、`load_factor`、`success_rate`、`avg_response_time_ms`、`is_available`、`weight`） | `{"status":"registered","agent_id"}` | 注册 Agent 参与调度 |
| `unregister_agent` | `agent_id` | `{"status":"unregistered","agent_id"}` | 注销 Agent（未找到也返回成功） |
| `schedule_task` | `task`（对象：`task_id`、`task_description`、`priority`、`timeout_ms` 默认 30000） | `{"task_id","status":"pending"}` | 异步入队（worker 随后真实派发） |
| `get_task` | `task_id` | 任务状态报告 JSON | 查询任务状态；未找到 -32603 |
| `cancel` | `task_id` | `{"task_id","status":"canceled"}` | 取消任务；不可取消 -32602 |
| `dag_submit` | `dag`（对象） | `{"dag_id","status":"active"}` | 提交 DAG；检测到环返回 -32602，超容量 -32602 |
| `dag_status` | `dag_id` | DAG 状态 JSON | 查询 DAG 状态 |
| `dag_cancel` | `dag_id` | `{"dag_id","status":"canceled"}` | 取消 DAG；非活动 -32602 |
| `submit` | 同 `schedule_task` | 同 `schedule_task` | `schedule_task` 的标准名别名 |
| `query` | 同 `get_task` | 同 `get_task` | `get_task` 的标准名别名 |
| `checkpoint_save` | 无 | 检查点 JSON | 保存调度检查点 |
| `health_check` | 无 | `{"service":"sched_d","healthy","timestamp"}` | L2 标准方法 |
| `get_stats` | 无 | 统计 JSON | daemon 级统计 |
| `plan` | `input`（字符串，必填） | `{"dispatch":"l1\|l2\|l3","result":...}` | 蓝图三级路由查询 |
| `absorb` | 模式 A：`plan`（对象 JSON，蓝图注册）；模式 B：`exec_id` + `node_id` + `output_json`/`result`/`verify`/`transient`/`canceled`/`is_user_intent`（执行结果回灌） | 模式 A：`{"status":"blueprint_registered"}`；模式 B：`{"status":"result_absorbed"}` | 蓝图注册或执行结果回灌（PASS+SUCCESS 写 L2 双写） |
| `roadmap_cancel` | `exec_id`（可选）、`node_id`（必填） | `{"status":"cancelled"}` | 取消事件注入（L1 回退 + L2 失效） |
| `roadmap_replan` | `affected_nodes`（非空数组，必填）、`replan_reason`（可选） | `{"status":"replanned","rerun_nodes":[...]}` | 蓝图修正（受影响节点回退 + L2 失效） |
| `roadmap_stats` | 无 | `{"ready":bool,"service":"sched_d.roadmap"}` | 蓝图实例状态 |

（另有标准方法 `shutdown`，不计入上述 18 个。）

## 配置

- 内置配置 `sched_config_t`：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `strategy` | `SCHED_STRATEGY_ROUND_ROBIN` | 调度策略（另有 weighted / priority_based / ml_based） |
| `health_check_interval_ms` | 5000 | 健康检查周期 |
| `stats_report_interval_ms` | 10000 | 统计上报周期 |
| `enable_ml_strategy` | false | 是否启用 ML 调度策略 |
| `max_agents` | 100 | Agent 注册上限 |
| `dag_max_parallel` | 0（串行） | DAG 并行派发上限（`AIRY_DAG_PARALLEL=N` 启用） |
| `dag_batch_size` | 0 | DAG 批大小 |
| `dag_fatal_cascade` | true | 仅 FATAL 级联取消全图 |

- 默认配置路径：`agentrt/manager/service/sched_d/sched.yaml`（`--manager`/`-c` 覆盖）。
- 环境变量：

| 环境变量 | 作用 |
|----------|------|
| `AIRY_DAG_FATAL_CASCADE` | `0` 时任何节点失败中止全图（恢复传统语义） |
| `AIRY_DAG_PARALLEL` | `N`（1..`SCHED_DAG_MAX_NODES`）启用 DAG 并行派发，N 为并发上限 |
| `AIRY_SCHED_DISPATCH` | `0` 关闭真实派发（任务入队但不执行） |
| `AIRY_SCHED_AGENT_SOCK` | 覆盖 agent_d 端点（默认 `$AIRY_RUNTIME_DIR/agent.sock`） |
| `AIRY_SCHED_DISPATCH_TIMEOUT_MS` | 派发超时（默认 300000） |

- 事件驱动：线程池 4~8，队列 256，`max_events=64`。

## 依赖与构建

- 依赖：`svc_common`；`airy_coreloopthree`（GNU ld 下 `--whole-archive`）、
  `airy_cognition` / `airy_core` / `airy_memory` / `airy_atoms` / `airy_common` /
  `airy_llm_service` / `airy_tool_service`、`Threads::Threads`、`airy_platform_libs`；
  引用 `monit_d/include/monitor_service.h`（monit 集成）。
- 编译撤销 `AIRY_USE_SCHEDULER_THREAD_IMPL`（corekern 调度器线程实现，sched_d 未初始化，
  恢复 commons 平台线程宏路径）。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target sched_d
```

## 测试

- `tests/`：`test_scheduler`（调度核心）、`test_strategies`（四策略）、
  `test_dag`（含按域拆分的 `test_dag_core` / `test_dag_failure` / `test_dag_parallel`，
  覆盖 DAG 解析/失败语义/并行派发），注册为 `sched_d_*` ctest 用例。
- 运行：

```bash
ctest --test-dir build -R "sched_d_" -V
```
