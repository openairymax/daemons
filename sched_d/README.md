# sched_d — 任务调度守护进程

> **模块路径**：`agentrt/daemons/sched_d/` · **可执行文件 / CMake 目标**：`sched_d` · **RPC 命名空间**：`sched.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`sched_d` 是 AgentRT 的任务调度守护进程：维护可调度 Agent 注册表，按策略为任务挑选执行者，
异步派发到 `agent_d` 真实执行，并支持 DAG 工作流与蓝图（roadmap）三级路由调度。
对外暴露 `sched.*` JSON-RPC 方法族。

- 端点：POSIX Unix socket `<runtime-dir>/sched.sock`（`$AIRY_HOME/run/sched.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8083`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8083`（默认只监听 socket）。

## 能力

- **Agent 注册表** — 注册 / 注销可调度 Agent，携带负载因子、成功率、平均响应时间与权重。
- **四种调度策略** — `round_robin`（默认）、`weighted`、`priority_based`、`ml_based`，
  策略以可插拔接口实现于 `src/strategies/`。
- **异步任务队列** — `schedule_task` 立即返回 `task_id` + `status=pending`，worker 线程随后
  完成「选 Agent → spawn → invoke → 状态写回」，用 `get_task` 查询进展。
- **真实派发** — 通过 `agent_d` 端点调用 `spawn` / `invoke` / `terminate`，失败如实上报，
  不以占位数据替代真实执行结果。
- **DAG 工作流** — 提交 / 查询 / 列举 / 取消；支持并行派发与失败分级级联语义。
- **蓝图调度** — 三级路由（状态机命中 → 语义缓存命中 → 全量规划）、蓝图注册与执行结果回灌。
- **检查点** — `checkpoint_save` 导出当前调度状态。

## 架构

```
gateway_d ──(sched.* JSON-RPC)──▶ sched_d
                                   │ sched_service（任务队列 + worker 线程）
                                   │   ├─ 策略：round_robin / weighted /
                                   │   │         priority_based / ml_based
                                   │   ├─ DAG 引擎（parse / engine / worker，
                                   │   │   串行或按 dag_max_parallel 并行）
                                   │   ├─ roadmap_sched（三级路由 + 语义缓存）
                                   │   └─ 执行器回调 sched_dispatch_executor
                                   ▼
                            <runtime-dir>/agent.sock
                            agent_d: spawn → invoke → terminate
```

- 事件驱动模型：`daemon_event_driver`，线程池 4~8、队列 256、最大事件 64；
- DAG 默认串行派发（`dag_max_parallel = 0`），设置 `AIRY_DAG_PARALLEL=N` 后由
  `atoms/coreloopthree` 的多 Agent 协作框架以 N 为并发上限并行推进；
- DAG 失败语义：默认仅 **FATAL** 级联取消整张图，普通失败不中断独立分支；
  `AIRY_DAG_FATAL_CASCADE=0` 恢复「任一节点失败即中止」的传统语义；
- 蓝图语义缓存独立持久化到 `<data-dir>/agentrt/roadmap/l2_semantic_cache.json`
  （POSIX 下 `AIRY_DATA_DIR` 默认为 `/var/lib/agentrt`），与命令行工具共用同一份缓存，
  因此规划结果可跨进程命中。

## JSON-RPC 接口

共 20 个方法，均由 `method_dispatcher_register` 注册。

**调度与 DAG（`src/main.c` + `src/sched_rpc_handlers.c`，15 个）**

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `register_agent` | `{agent:{agent_id, agent_name?, load_factor?, success_rate?, avg_response_time_ms?, is_available?, weight?}}` | `{status:"registered", agent_id}` | 注册 Agent 参与调度（`agent_id` 必填） |
| `unregister_agent` | `{agent_id}` | `{status:"unregistered", agent_id}` | 注销 Agent；不存在也返回成功 |
| `schedule_task` | `{task:{task_id?, task_description?, priority?, timeout_ms?}}` | `{task_id, status:"pending"}` | 异步入队；`task_id` 与 `task_description` 全缺时拒绝（`timeout_ms` 默认 30000） |
| `get_task` | `{task_id}` | 任务状态报告 JSON | 查询任务状态；未找到返回内部错误 |
| `cancel` | `{task_id}` | `{task_id, status:"canceled"}` | 取消任务；不可取消（运行中）返回参数错误 |
| `dag_submit` | `{dag:{…}}` | `{dag_id, status:"active"}` | 提交 DAG；检出环或超容量返回参数错误 |
| `dag_status` | `{dag_id}` | DAG 状态与逐节点明细 JSON | 查询单个 DAG |
| `dag_list` | `{}` | `{dags:[{dag_id, name, status, node_count, progress, created_at_ms, finished_at_ms}], count}` | 轻量看板：列举全部 DAG（不含逐节点明细） |
| `dag_cancel` | `{dag_id}` | `{dag_id, status:"canceled"}` | 取消 DAG；不存在或非活动返回参数错误 |
| `submit` | 同 `schedule_task` | 同 `schedule_task` | `schedule_task` 别名 |
| `query` | 同 `get_task` | 同 `get_task` | `get_task` 别名 |
| `checkpoint_save` | `{}` | 检查点 JSON | 保存并返回调度检查点 |
| `health_check` | `{}` | `{service:"sched_d", healthy, timestamp}` | 服务健康检查（`timestamp` 为毫秒） |
| `get_stats` | `{}` | 调度统计 JSON | daemon 级统计 |
| `shutdown` | `{}` | — | 优雅退出 |

**蓝图调度（`src/roadmap_rpc.c`，5 个）**

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `plan` | `{input}`（字符串，必填） | `{dispatch:"l1"\|"l2"\|"l3", result}` | 三级路由查询；`result` 是规划结果的 JSON **字符串**；未初始化返回内部错误 |
| `absorb` | 形态 A：`{exec_id?, plan:{…}}` 注册蓝图；形态 B：`{exec_id, node_id, output_json?, result?, verify?, transient?, canceled?, is_user_intent?}` 回灌执行结果 | 形态 A：`{status:"blueprint_registered"}`；形态 B：`{status:"result_absorbed"}` | 蓝图注册 / 执行结果回灌（成功且校验通过的结果写入语义缓存） |
| `roadmap_cancel` | `{node_id, exec_id?}` | `{status:"cancelled"}` | 取消事件注入：状态机回退 + 缓存失效 |
| `roadmap_replan` | `{affected_nodes:[…], replan_reason?}` | `{status:"replanned", rerun_nodes:[…]}` | 蓝图修正：受影响节点回退 + 缓存失效，返回需重跑节点 |
| `roadmap_stats` | `{}` | `{ready, service:"sched_d.roadmap"}` | 蓝图调度实例是否可用 |

外部调用方使用带命名空间前缀的形式（`sched.schedule_task`），由 `gateway_d` 剥离前缀后转发。

## 配置

调度参数为进程内建默认值，写在 `src/main.c` 中：

| 字段 | 默认 | 说明 |
|------|------|------|
| `strategy` | `SCHED_STRATEGY_ROUND_ROBIN` | 另有 `weighted` / `priority_based` / `ml_based` |
| `health_check_interval_ms` | 5000 | 健康检查周期 |
| `stats_report_interval_ms` | 10000 | 统计上报周期 |
| `enable_ml_strategy` | `false` | 是否启用 ML 调度策略 |
| `ml_model_path` | `NULL` | ML 策略模型路径 |
| `max_agents` | 100 | Agent 注册上限 |
| `dag_max_parallel` | 0（串行） | DAG 并行派发上限，由 `AIRY_DAG_PARALLEL` 覆盖 |
| `dag_batch_size` | 0 | DAG 批大小 |
| `dag_fatal_cascade` | `true` | 仅 FATAL 级联取消整图，由 `AIRY_DAG_FATAL_CASCADE` 覆盖 |

命令行选项与其他守护进程一致：`--manager <config>` 指定配置路径、`--tcp` 切换到
TCP 监听、`--help` 打印用法。**当前版本 `--manager` 给出的路径只记录到启动日志，不会加载**，
调度参数一律取上表内建值。

| 环境变量 | 默认 | 说明 |
|----------|------|------|
| `AIRY_SCHED_SOCK` | — | 网关侧覆盖 sched_d 端点 |
| `AIRY_SCHED_DISPATCH` | 开启 | 置 `0` 关闭真实派发（任务入队但不执行） |
| `AIRY_SCHED_AGENT_SOCK` | `<runtime-dir>/agent.sock` | 覆盖 agent_d 端点 |
| `AIRY_SCHED_DISPATCH_TIMEOUT_MS` | 300000 | 单次派发超时 |
| `AIRY_DAG_FATAL_CASCADE` | — | 置 `0` 时任一节点失败即中止全图 |
| `AIRY_DAG_PARALLEL` | — | 置 `N`（1..`SCHED_DAG_MAX_NODES`）启用并行派发，非法值回退串行 |
| `AIRY_HOME` / `AIRY_RUNTIME_DIR` / `AIRY_DATA_DIR` | — | 运行目录与数据目录 |

## 用法

```bash
airymaxrt logs sched_d        # 运行态日志
<build-dir>/bin/sched_d       # 手工启动单个进程（监听运行目录）
```

真实派发依赖 `agent_d` 同时在跑；只想观察排队与调度行为时可临时关闭派发：

```bash
AIRY_SCHED_DISPATCH=0 <build-dir>/bin/sched_d
```

直连端点验证（POSIX）：

```bash
SOCK="${AIRY_HOME:-$HOME/.airymaxrt}/run/sched.sock"
printf '%s' '{"jsonrpc":"2.0","id":1,"method":"get_stats","params":{}}' | socat - UNIX-CONNECT:"$SOCK"
printf '%s' '{"jsonrpc":"2.0","id":2,"method":"dag_list","params":{}}' | socat - UNIX-CONNECT:"$SOCK"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target sched_d
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

`BUILD_TESTS=ON` 时注册 3 个 CTest 用例（DAG 用例按功能域拆分为
`test_dag_core` / `test_dag_failure` / `test_dag_parallel`，与 `test_dag.c` 同编译单元）：

| 用例 | 覆盖点 |
|------|--------|
| `sched_d_test_scheduler` | 任务队列、Agent 注册表、worker 派发 |
| `sched_d_test_strategies` | 四种调度策略选择行为 |
| `sched_d_test_dag` | DAG 解析、状态看板、失败语义、并行派发 |

```bash
ctest --test-dir ../daemons-build -R sched_d --output-on-failure
```

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher、RPC 客户端 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、同步、cJSON 封装 |
| [atoms](https://atomgit.com/openairymax/atoms) | `corekern`（`airy_init()`）、`coreloopthree`（DAG 并行协作）、`cognition`、`memory` |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 能力层（经 `svc_common` 传递） |
| [daemons](https://atomgit.com/openairymax/daemons) | `airy_llm_service` / `airy_tool_service` 服务适配层 |

sched_d 的全部源文件直接编入可执行文件（不额外抽服务静态库），单元测试通过重复编译
`src/` 内的实现文件覆盖真实代码路径。GNU ld 下 `airy_coreloopthree` 以 `--whole-archive`
强制纳入；Windows 额外链接 `ws2_32`、`bcrypt`。

## 关系

`sched_d` 只做**决策与排队**，不亲自执行任务：真正拉起并驱动 Agent 的是
[`agent_d`](../agent_d/README.md)（`spawn` / `invoke` / `terminate`）。
[`think_d`](../think_d/README.md) 负责认知流程编排，[`tool_d`](../tool_d/README.md)
负责工具调用执行；任务完成后的对外事件由 [`notify_d`](../notify_d/README.md) 广播，
调度指标经 `gateway_d` 上报给 [`monit_d`](../monit_d/README.md)。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
