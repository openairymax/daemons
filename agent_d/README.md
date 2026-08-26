# agent_d — Agent 执行守护进程

> 命名空间：`agent.*`
> Unix socket：`${AIRY_RUNTIME_DIR}/agent.sock`（Windows：`\\.\pipe\airy_agent`）
> TCP 端口：8086（`--tcp` 启用，默认仅 Unix socket）

## 定位

`agent_d` 承载 AgentRT 的 Agent 编排执行：负责 Agent 子进程的生成（spawn）、
调用（invoke）、终止（terminate）、取消（cancel）与生命周期管理。将原
`syscall_router.c` 的 `airy_sys_agent_spawn/terminate/invoke/list` 进程内联实现
抽离为独立 daemon，`gateway_d` 通过 Unix socket IPC 转发 `agent.*` 请求。

关键能力：

- **spawn 后自动注册 sched_d**：`agent.spawn` 成功后以 `agent_spec.role` 为 key
  向 sched_d（`sched.sock`）调用 `register_agent`，解决"调度注册表恒为空、
  schedule_task 无 Agent 可选"的问题；注册失败仅告警不阻塞 spawn。
- **跨进程取消（agent.cancel）**：`agent.invoke` 可携带 `request_id` 注册会话，
  调用方经 `agent.cancel` 按 request_id 取消——服务层经 select 轮询感知
  cancel token，SIGTERM → 2s → SIGKILL 终止子进程，原 invoke 返回 AbortedOutput
  （与超时路径可区分）。`concurrent_clients` 开启，保证长 invoke（LLM 往返最长
  约 300s）不阻塞事件循环、cancel 请求可达。
- **idle reaper（空闲收割）**：守护线程每 30s 扫描一次，回收空闲超过阈值
  （`AIRY_AGENT_IDLE_TIMEOUT_S`，默认 300s）的 Agent 子进程，防止 Python runner
  进程泄漏。
- **性能监控**：采样线程按 `AIRY_AGENT_PERF_INTERVAL_S`（默认 5s）聚合
  spawn/invoke/terminate 窗口增量、平均/最大时延（微秒）、全局锁争用计数、
  线程池深度等，输出一行 `[PERF]` 日志；单请求超过
  `AIRY_AGENT_PERF_SLOW_US`（默认 1000000μs）记录慢调用告警。

## 架构

```
gateway_d ──(agent.spawn / agent.invoke / agent.cancel)──▶ agent_d ──▶ Agent 子进程
      │                                                        │
      │                                                        ├─ service_spawn.c  子进程生成
      │                                                        ├─ service_invoke.c 调用/取消
      │                                                        ├─ service_child.c  子进程生命周期
      │                                                        └─ service_stats.c  性能统计
      └─ spawn 成功后 ──(register_agent)──▶ sched_d（sched.sock）
```

- 服务层 `src/service.c`（含 service_spawn/service_invoke/service_child/service_stats
  拆分）经 `agent_svc_adapter.c` 适配到 `airy_svc_t` 统一服务管理框架。
- spawn 出的 Agent 由子进程承载（`agent_spec` 为 JSON 字符串，含 `type`/`model`
  等字段）；测试环境可用 `AIRY_AGENT_NO_SPAWN=1` 走确定性模式（不 fork 真实子进程）。
- 会话表容量 `AGENT_INVOKE_SESSIONS_MAX`，满时 `invoke_begin` 返回 BUSY。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `agent.spawn` | `{agent_spec: string 或 object}` | `{agent_id}` | 按 spec 生成 Agent 子进程，成功后自动注册 sched_d |
| `agent.terminate` | `{agent_id: string}` | `{terminated: true}` | 终止指定 Agent（槽位不回收，count 不变） |
| `agent.invoke` | `{agent_id, input, workspace_dir?: string, request_id?: string}` | `{output}` | 调用 Agent；`request_id` 用于跨进程取消会话 |
| `agent.cancel` | `{request_id: string}` | `{canceled: true}` | 按 request_id 取消活跃 invoke 会话 |
| `agent.list` | `{}` | `{agent_ids: [string], total}` | 列出全部 Agent ID |
| `agent.count` | `{}` | `{count}` | 当前 Agent 数 |
| `agent.health_check` | `{}` | `{service, healthy, agents, timestamp}` | 服务健康检查 |
| `agent.get_stats` | `{}` | `{daemon, uptime_s, agents, spawn_total/ok/fail, invoke_total/ok/fail, terminate_total, lock_wait_total, peak_running, avg_spawn_us, avg_invoke_us}` | 服务与性能统计 |
| `agent.shutdown` | `{}` | — | 优雅退出 |

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/agent.sock",
    "tcp_port": 8086,
    "max_clients": 2048,
    "max_agents": 10000
  }
}
```

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_MAX_AGENTS` | 10000 | Agent 容量上限（< 65536 生效） |
| `AIRY_AGENT_IDLE_TIMEOUT_S` | 300 | 空闲收割阈值（秒），0 表示立即收割 |
| `AIRY_AGENT_PERF_INTERVAL_S` | 5 | [PERF] 聚合采样间隔（秒） |
| `AIRY_AGENT_PERF_SLOW_US` | 1000000 | 慢调用告警阈值（微秒） |
| `AIRY_AGENT_NO_SPAWN` | 未设置 | 设为 1 时进入确定性模式（不 fork 真实子进程，供测试） |

## 依赖与构建

依赖：`svc_common`（daemon 公共层）、`airy_common`、cJSON、`cancel_token`
（取消令牌，atoms 层）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target agent_d
```

安装：`cmake --install` 将 `agent_d` 装入 `bin`，头文件装入 `include/agentrt`。

## 测试

```bash
ctest --test-dir build -R agent_d_ --output-on-failure
```

单元测试（`tests/test_service.c` → `test_agent_service`，以
`AIRY_AGENT_NO_SPAWN=1` 确定性模式运行，不 fork 真实 Python 子进程）覆盖：

- 创建/销毁、spawn/list、terminate（terminated 后再 invoke 返回错误）、
  invoke 不存在 Agent（NOT_FOUND）；
- 容量上限与"终止后不回收槽位、容量内可再 spawn"的语义；
- 取消链路：cancel token 命中 → 子进程终止 → invoke 返回 `AIRY_ERR_CANCELED`
  （输出含 "aborted"）；
- 会话取消：`invoke_begin` / `agent_service_invoke_cancel` / `invoke_end`
  按 request_id 的完整闭环，以及会话表容量上限（溢出返回 BUSY）。
