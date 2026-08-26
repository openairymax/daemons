# agent_d — Agent 执行守护进程（agent.* 命名空间）

> **命名空间**：`agent.*`
> **Unix socket**：`${AIRY_RUNTIME_DIR}/agent.sock`

## 职责

`agent_d` 承载 AgentRT 的 **Agent 编排执行**：负责 Agent 子进程的生成
（spawn）、调用（invoke）与健康检查。从原 gateway 进程内联实现抽离为独立
daemon，获得进程隔离与故障隔离能力。

- `agent.spawn`：按 Agent 描述（能力、模型、RBAC）创建 Agent 子进程；
- `agent.invoke`：向已生成的 Agent 子进程投递任务并取回结果；
- `agent.health_check`：检查 Agent 子进程存活状态。

`gateway_d` 通过 Unix socket IPC（`airy_sys_svc_call`）转发 `agent.*` 请求到
`agent_d`，保持 C ABI 兼容。

## JSON-RPC 方法

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `agent.spawn` | `{spec: object}` | `{agent_id, status}` | 生成 Agent 子进程 |
| `agent.invoke` | `{agent_id, task}` | `{result}` | 调用 Agent 执行任务 |
| `agent.health_check` | `{agent_id?}` | `{status}` | Agent/服务健康检查 |

## 架构

```
gateway_d ──(agent.spawn / agent.invoke)──▶ agent_d ──▶ Agent 子进程
      │                                        │            │
      │                                        ├─ service_spawn.c
      │                                        ├─ service_invoke.c
      │                                        └─ service_child.c（子进程生命周期）
```

## 配置

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/agent.sock",
    "max_clients": 64
  }
}
```

## 构建与测试

```bash
cmake --build ${AIRY_HOME}/build --target agent_d
```
