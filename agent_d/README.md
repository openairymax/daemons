# agent_d — Agent 执行守护进程

> **模块路径**：`agentrt/daemons/agent_d/` · **可执行文件 / CMake 目标**：`agent_d` · **RPC 命名空间**：`agent.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`agent_d` 承载 AgentRT 的 Agent 生命周期与执行循环：生成（spawn）、调用（invoke）、
运行一轮完整对话（run）、取消（cancel / run_cancel）、终止（terminate）与统计。
`gateway_d` 按 `agent.*` 命名空间转发请求，Agent 实体由本进程生成的子进程承载。

- 端点：POSIX Unix socket `<runtime-dir>/agent.sock`（`$AIRY_HOME/run/agent.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8086`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8086`。

## 能力

- **进程级 Agent 隔离** — 每个 Agent 由独立子进程运行，`agent_spec` 为 JSON
  （含 `type` / `model` 等字段）。
- **spawn 后自动注册调度** — `spawn` 成功后以 `agent_spec.role` 为 key 调用
  `sched_d` 的 `register_agent`，使调度器立即可选到该 Agent；注册失败仅告警，
  不阻塞 spawn。
- **跨进程取消** — `invoke` 可携带 `request_id` 注册会话，调用方经 `cancel` 按
  `request_id` 取消；服务层经 select 轮询感知取消令牌，`SIGTERM` → 2s → `SIGKILL`
  终止子进程，原 `invoke` 返回可区分的 AbortedOutput。
- **流式执行** — `run_stream` 逐帧推送执行事件，客户端以连接关闭作为 EOF。
- **空闲收割** — 守护线程每 30s 扫描，回收空闲超过
  `AIRY_AGENT_IDLE_TIMEOUT_S`（默认 300s）的子进程，防止进程泄漏。
- **性能采样** — 按 `AIRY_AGENT_PERF_INTERVAL_S`（默认 5s）聚合窗口增量、
  平均 / 最大时延、锁争用、线程池深度，输出一行 `[PERF]` 日志；单请求超过
  `AIRY_AGENT_PERF_SLOW_US`（默认 1000000 μs）记录慢调用告警。
- **确定性测试模式** — `AIRY_AGENT_NO_SPAWN=1` 时不 fork 真实子进程。

## 架构

```
gateway_d ──(agent.spawn / invoke / run / run_stream / cancel)──▶ agent_d ──▶ Agent 子进程
      │                                                             │
      │                                                             ├─ service_spawn.c  子进程生成
      │                                                             ├─ service_invoke.c 调用/取消
      │                                                             ├─ service_child.c  子进程生命周期
      │                                                             ├─ agent_run_rpc.c  run 引擎适配
      │                                                             └─ service_stats.c  性能统计
      └─ spawn 成功后 ──(register_agent)──▶ sched_d
```

服务层经 `agent_svc_adapter.c` 适配到 `airy_svc_t` 统一服务框架。会话表容量为
`AGENT_INVOKE_SESSIONS_MAX`，满时 `invoke_begin` 返回 BUSY。`concurrent_clients`
开启，保证长 `invoke`（LLM 往返可达数百秒）不阻塞事件循环、取消请求仍可达。

## JSON-RPC 接口

共 12 个方法，经 `method_dispatcher_register` 注册：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `spawn` | `{agent_spec: string 或 object}` | `{agent_id}` | 按 spec 生成 Agent 子进程，成功后自动注册 sched_d |
| `terminate` | `{agent_id: string}` | `{terminated: true}` | 终止指定 Agent（槽位不回收，`count` 不变） |
| `invoke` | `{agent_id, input, workspace_dir?, request_id?}` | `{output}` | 调用 Agent；`request_id` 用于跨进程取消会话 |
| `cancel` | `{request_id}` | `{canceled: true}` | 按 `request_id` 取消活跃 invoke 会话 |
| `run` | `{prompt \| messages, model?, gccp_answers?, agent?, agent_file?, session_id?}` | 执行结果 JSON | 运行一轮完整执行循环（工具往返 + LLM） |
| `run_cancel` | `{session_id}` | `{status:"cancelling", session_id}` | 按 `session_id` 取消活跃 run 会话 |
| `run_stream` | 同 `run` | 事件帧流 | 流式执行，逐帧推送；连接关闭即 EOF |
| `list` | `{}` | `{agent_ids: [string], total}` | 列出全部 Agent ID |
| `count` | `{}` | `{count}` | 当前 Agent 数 |
| `health_check` | `{}` | `{service, healthy, agents, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, uptime_s, agents, spawn_total/ok/fail, invoke_total/ok/fail, terminate_total, lock_wait_total, peak_running, avg_spawn_us, avg_invoke_us}` | 服务与性能统计 |
| `shutdown` | `{}` | — | 优雅退出 |

`run` / `run_stream` 的 `prompt` 缺失时回落取 `messages[0].content`；两者都不满足返回
`-32602`。取消命中已结束的会话返回 `-32601`。

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/agent.sock",
    "tcp_port": 8086,
    "max_clients": 2048,
    "max_agents": 10000
  }
}
```

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_MAX_AGENTS` | 10000 | Agent 容量上限（`< 65536` 生效） |
| `AIRY_AGENT_IDLE_TIMEOUT_S` | 300 | 空闲收割阈值（秒），`0` 表示立即收割 |
| `AIRY_AGENT_PERF_INTERVAL_S` | 5 | `[PERF]` 聚合采样间隔（秒） |
| `AIRY_AGENT_PERF_SLOW_US` | 1000000 | 慢调用告警阈值（微秒） |
| `AIRY_AGENT_NO_SPAWN` | 未设置 | 设为 `1` 进入确定性模式（不 fork 子进程，供测试） |
| `AIRY_AGENT_SOCK` | — | 调用方覆盖 agent_d 端点 |

Agent 子进程的 stderr 重定向到运行目录下的 `agent_<agent_id>.log`，用于排查崩溃。

## 用法

```bash
airymaxrt logs agent_d        # 运行态日志
<build-dir>/bin/agent_d       # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target agent_d
ctest --test-dir ../daemons-build -R agent_d_ --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

单元测试（`tests/test_service.c` → `test_agent_service`，以 `AIRY_AGENT_NO_SPAWN=1`
确定性模式运行）覆盖：创建 / 销毁、spawn / list、terminate（终止后再 invoke 返回错误）、
invoke 不存在的 Agent（NOT_FOUND）、容量上限与「终止后不回收槽位、容量内可再 spawn」语义、
取消链路（令牌命中 → 子进程终止 → 返回 `AIRY_ERR_CANCELED`）、以及
`invoke_begin` / `invoke_cancel` / `invoke_end` 按 `request_id` 的完整闭环与会话表溢出 BUSY。

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON 封装 |
| [atoms](https://atomgit.com/openairymax/atoms) | `cancel_token` 取消令牌 |
| [sched_d](../sched_d/README.md) | 下游：spawn 成功后注册可调度 Agent |
| [llm_d](../llm_d/README.md) | 下游：run 循环中的模型往返 |

## 关系

`agent_d` 管「一个 Agent 进程怎么活」；`sched_d` 管「哪个 Agent 该干活」；
`a2a_d` 管「跨 Agent 怎么通信」；`think_d` 管「一次交互怎么想」。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
