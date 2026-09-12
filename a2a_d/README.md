# a2a_d — Agent 间通信（A2A）守护进程

> **模块路径**：`agentrt/daemons/a2a_d/` · **可执行文件 / CMake 目标**：`a2a_d` · **RPC 命名空间**：`a2a.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`a2a_d` 承载 AgentRT 的 **Agent-to-Agent（A2A）协议通信**：多 Agent 协作场景下的
Agent Card 注册与发现、任务生命周期管理与消息交换。协议转换在 `gateway_d` 边界完成，
`a2a_d` 只处理内部 JSON-RPC 请求。

- 端点：POSIX Unix socket `<runtime-dir>/a2a.sock`（`$AIRY_HOME/run/a2a.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8087`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8087`（默认只监听 socket）。

## 能力

- **Agent Card 注册 / 发现** — 整段 Card（id / name / description / capabilities /
  skills 等）注册，可按 capability 或 skill 过滤发现，可注销。
- **任务状态机** — 创建 / 推进 / 取消 / 查询，`state` 为 `a2a_task_state_t` 枚举值。
- **消息投递** — 向目标 Agent 发送消息并收集响应。
- **别名兼容** — `send` 等价 `send_message`、`receive` 等价 `get_task`。
- **独立容量上限** — Agent 注册表（默认 256）与任务表（默认 4096）分别限额。

## 架构

```
gateway_d ──(a2a.register_agent / create_task / send_message)──▶ a2a_d
      │                                                           │
      │  A2A 协议转换在网关边界完成                                ├─ Agent Card 注册表
      │                                                           ├─ 任务表（生命周期状态机）
      └──────────────── 寻址 + 消息路由 ◀────────────────────────┘
```

服务层 `src/service.c` + `src/a2a_svc_adapter.c` 抽为静态库 `airy_a2a_service`，
被守护进程可执行文件与单元测试共用。

## JSON-RPC 接口

共 14 个方法，经 `method_dispatcher_register` 注册：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `register_agent` | `{id, name, description?, url?, version?, protocol_version?: int, capabilities?: int, available?: bool, skills?: array}` | `{registered: true, agent_id}` | 注册 Agent（整个 params 作为 Agent Card 序列化；缺省字段取库默认值） |
| `unregister_agent` | `{agent_id}` | `{unregistered: true}` | 注销 Agent |
| `discover_agents` | `{capability?: string, skill?: string}` | `{agents: [card...], count}` | 按能力 / 技能过滤发现 Agent |
| `create_task` | `{agent_id, description, input?: string}` | `{task}` | 为指定 Agent 创建任务 |
| `update_task` | `{task_id, state: int, output?: string, progress?: number}` | `{updated: true}` | 推进任务状态（`progress` 取值 `[0.0, 1.0]`） |
| `cancel_task` | `{task_id, reason?: string}` | `{canceled: true}` | 取消任务 |
| `get_task` | `{task_id}` | `{task}` | 查询任务 |
| `send_message` | `{target_agent_id, role, content}` | `{responses: [...], count}` | 向目标 Agent 发消息并收集响应 |
| `send` | 同 `send_message` | 同 `send_message` | `send_message` 别名 |
| `receive` | 同 `get_task` | 同 `get_task` | `get_task` 别名 |
| `count` | `{}` | `{agent_count, task_count}` | Agent / 任务计数 |
| `health_check` | `{}` | `{service, healthy, agent_count, task_count, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, agents, tasks}` | 服务统计 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`a2a.register_agent`），由 `gateway_d` 剥离前缀后转发。

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/a2a.sock",
    "tcp_port": 8087,
    "max_clients": 64,
    "max_agents": 256,
    "max_tasks": 4096
  }
}
```

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_A2A_MAX_AGENTS` | 256 | Agent 注册表上限（`< 65536` 生效） |
| `AIRY_A2A_MAX_TASKS` | 4096 | 任务表上限（`< 1048576` 生效） |
| `AIRY_A2A_SOCK` | — | 网关侧覆盖 a2a_d 端点 |

## 用法

```bash
airymaxrt logs a2a_d          # 运行态日志
<build-dir>/bin/a2a_d         # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target a2a_d
ctest --test-dir ../daemons-build -R a2a_d_ --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON 封装 |
| [protocols](https://atomgit.com/openairymax/protocols) | `a2a_v03_adapter` —— A2A 协议实现 |
| [gateway_d](../gateway_d/README.md) | 上游：`a2a.*` 命名空间转发方 |

## 关系

`a2a_d` 与 `agent_d`（本机 Agent 生命周期）、`sched_d`（任务派发）分工不同：
`a2a_d` 只负责跨 Agent 的协议层——Card 目录、任务状态机、消息路由。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
