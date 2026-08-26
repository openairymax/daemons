# a2a_d — Agent 间通信守护进程（a2a.* 命名空间）

> **命名空间**：`a2a.*`
> **Unix socket**：`${AIRY_RUNTIME_DIR}/a2a.sock`

## 职责

`a2a_d` 承载 AgentRT 的 **Agent-to-Agent（A2A）协议通信**，实现多 Agent
协作场景下的注册、任务创建与消息交换。从原 gateway 进程内联实现抽离为
独立 daemon，获得进程隔离与故障隔离能力。

- `a2a.register_agent`：注册一个可被其他 Agent 寻址的 Agent；
- `a2a.create_task`：创建跨 Agent 任务；
- `a2a.send_message`：向目标 Agent 发送消息。

`gateway_d` 通过 Unix socket IPC（`airy_sys_svc_call`）转发 `a2a.*` 请求到
`a2a_d`，保持 C ABI 兼容。A2A 协议转换在 gateway 边界完成，a2a_d 只负责
进程内的寻址与消息路由。

## JSON-RPC 方法

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `a2a.register_agent` | `{agent_id, capabilities?}` | `{ok}` | 注册 Agent 地址 |
| `a2a.create_task` | `{task: object}` | `{task_id}` | 创建跨 Agent 任务 |
| `a2a.send_message` | `{to, from, content}` | `{ok}` | 向目标 Agent 发送消息 |
| `a2a.health_check` | `{}` | `{status}` | 服务健康检查 |

## 架构

```
gateway_d ──(a2a.register / a2a.send_message)──▶ a2a_d ──▶ 目标 Agent
      │                                              │
      │         A2A 协议转换在 gateway 边界           └─ 寻址 + 消息路由
```

## 配置

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/a2a.sock",
    "max_clients": 64
  }
}
```

## 构建与测试

```bash
cmake --build ${AIRY_HOME}/build --target a2a_d
```
