# a2a_d — Agent 间通信（A2A）守护进程

> 命名空间：`a2a.*`
> Unix socket：`${AIRY_RUNTIME_DIR}/a2a.sock`（Windows：`\\.\pipe\airy_a2a`）
> TCP 端口：8087（`--tcp` 启用，默认仅 Unix socket）

## 定位

`a2a_d` 承载 AgentRT 的 **Agent-to-Agent（A2A）协议通信**，实现多 Agent 协作
场景下的 Agent 注册与发现、任务生命周期管理（创建/更新/取消/查询）与消息交换。
服务核心封装自 `a2a_v03_adapter` 库，`gateway_d` 通过 Unix socket IPC 转发
`a2a.*` 请求，A2A 协议转换在 gateway 边界完成。

关键能力：

- **Agent Card 注册/发现**：`a2a.register_agent` 以整段 Agent Card（id/name/
  description/capabilities/skills 等）注册；`a2a.discover_agents` 可按
  capability 或 skill 过滤发现；`a2a.unregister_agent` 注销。
- **任务生命周期**：`create_task` / `update_task`（状态机推进，state 为
  `a2a_task_state_t` 枚举值）/ `cancel_task` / `get_task`。
- **消息投递**：`a2a.send_message` 向目标 Agent 发送消息并收集响应。
- **L2 别名**：`a2a.send` 与 `a2a.send_message` 同义、`a2a.receive` 与
  `a2a.get_task` 同义，兼容 L2 服务协议（`02-l2-service-protocol.md`）标准方法。

## 架构

```
gateway_d ──(a2a.register_agent / create_task / send_message)──▶ a2a_d
      │                                                           │
      │  A2A 协议转换在 gateway 边界                              ├─ 注册表（Agent Card）
      │                                                           ├─ 任务表（生命周期状态机）
      └──────────── 寻址 + 消息路由 ◀────────────────────────────┘
```

- 服务层 `src/service.c` + `src/a2a_svc_adapter.c` 抽为静态库
  `airy_a2a_service`，与 daemon 可执行文件及单元测试共用。
- 容量：Agent 注册表（默认 256）与任务表（默认 4096）独立上限。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `a2a.register_agent` | `{id, name, description?, url?, version?, protocol_version?: int, capabilities?: int, available?: bool, skills?: array}` | `{registered: true, agent_id}` | 注册 Agent（整个 params 作为 Agent Card 序列化；缺省字段取库默认值） |
| `a2a.unregister_agent` | `{agent_id}` | `{unregistered: true}` | 注销 Agent |
| `a2a.discover_agents` | `{capability?: string, skill?: string}` | `{agents: [card...], count}` | 按能力/技能过滤发现 Agent |
| `a2a.create_task` | `{agent_id, description, input?: string}` | `{task}` | 为指定 Agent 创建任务 |
| `a2a.update_task` | `{task_id, state: int, output?: string, progress?: number}` | `{updated: true}` | 推进任务状态（state 为 a2a_task_state_t 枚举，progress 取值 [0.0, 1.0]） |
| `a2a.cancel_task` | `{task_id, reason?: string}` | `{canceled: true}` | 取消任务 |
| `a2a.get_task` | `{task_id}` | `{task}` | 查询任务 |
| `a2a.send_message` | `{target_agent_id, role, content}` | `{responses: [...], count}` | 向目标 Agent 发消息并收集响应 |
| `a2a.send` | 同 `send_message` | 同 `send_message` | `send_message` 的 L2 别名 |
| `a2a.receive` | 同 `get_task` | 同 `get_task` | `get_task` 的 L2 别名 |
| `a2a.count` | `{}` | `{agent_count, task_count}` | Agent/任务计数 |
| `a2a.health_check` | `{}` | `{service, healthy, agent_count, task_count, timestamp}` | 服务健康检查 |
| `a2a.get_stats` | `{}` | `{daemon, agents, tasks}` | 服务统计 |
| `a2a.shutdown` | `{}` | — | 优雅退出 |

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/a2a.sock",
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
| `AIRY_A2A_MAX_AGENTS` | 256 | Agent 注册表上限（< 65536 生效） |
| `AIRY_A2A_MAX_TASKS` | 4096 | 任务表上限（< 1048576 生效） |

## 依赖与构建

依赖：`svc_common`（daemon 公共层）、`airy_common`、cJSON、
`a2a_v03_adapter`（A2A 协议库）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target a2a_d
```

安装：`cmake --install` 将 `a2a_d` 装入 `bin`，头文件装入 `include/agentrt`。

## 测试

```bash
ctest --test-dir build -R a2a_d_ --output-on-failure
```

单元测试（`tests/test_service.c` → `test_a2a_service`）覆盖：

- 创建/销毁；
- Agent 注册与按 capability/skill 发现、注销、按 ID 取回 Agent Card；
- 任务创建、消息发送（含响应收集）、agent/task 计数。
