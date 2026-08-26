# notify_d — 多协议通知守护进程（notify.* 命名空间）

> **模块路径**: `agentrt/daemons/notify_d/`
> **命名空间**: `notify.*`（gateway 转发时剥离前缀，方法名不带 `notify.`）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/notify.sock`（Linux）；Windows 走 TCP `127.0.0.1:8084`

## 定位

`notify_d` 是 AgentRT 的多协议通知守护进程，向外部客户端提供 WebSocket、SSE 与
Unix Socket 三种事件订阅/广播通道，内置频道订阅注册表、环形事件队列（容量 1024）
与后台广播线程，是 AgentRT 事件驱动架构的通知枢纽。

## 架构

```
事件源（各守护进程 / JSON-RPC publish）
        ↓
  事件队列（ring buffer，容量 1024，后台线程消费）
        ↓
  广播引擎（频道匹配 + 订阅注册表匹配）
        ├── WebSocket 客户端（文本帧 0x81）
        ├── SSE 客户端（event:/data: 格式）
        └── Unix Socket 客户端（原始 JSON）
```

- 网络层（`main.c`）：自定义 accept 循环 + 每连接独立线程（上限 128 并发连接，
  超出直接拒绝）；WebSocket 握手内嵌 SHA-1 + Base64（GUID
  `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`），无外部加密依赖；
- 服务核心（`notify_service.c`）：订阅注册表（512 上限）、事件入队/广播、
  JSON-RPC 分发（`notify_d_dispatch_jsonrpc`）；
- 客户端身份：连接时可通过 `X-Client-Id` 头声明身份；广播投递匹配「连接频道
  （`X-Channel`）」或「订阅注册表 (channel, client_id)」；
- `main()` 不解析命令行参数（`argc/argv` 未使用），不接受 `--config`；
- 健康判定：队列满或错误率 > 已通知数一半（且已通知 >10）判为不健康。

## JSON-RPC 接口

以下方法表以 `notify_service.c` 中 `notify_d_dispatch_jsonrpc` 的真实分发为准
（共 8 个方法）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `publish` | `message` 或 `payload`(必填)、`channel`(默认 "default")、`event`(默认 "message") | 事件入队广播，返回 `{queued:true,channel,event,pending,subscribers}` |
| `subscribe` | `channel`(必填)、`client_id`(必填) | 加入订阅注册表（幂等），返回 `{status:"subscribed",channel,client_id,subscribers}` |
| `unsubscribe` | `channel`(必填)、`client_id`(必填) | 移出订阅注册表（幂等），返回 `{status:"unsubscribed",...}` |
| `list` | — | `{clients,subscriptions,channels:[{channel,subscribers,active_clients}]}` |
| `health` | — | `{status,service,queue_pending,queue_capacity,queue_occupancy,consumer_running,active_clients,subscriptions,notified,errors,uptime_s}` |
| `get_stats` | — | `{daemon:"notify_d",uptime_s,notified,errors,clients,pending}` |
| `health_check` | — | `{status:"ok",service:"notify_d",uptime_s,timestamp}` |
| `shutdown` | — | 置位退出标志，返回 `{status:"shutting_down"}` |

## 配置

- 常量（编译期，`notify_service.h` / `main.c`）：
  `NOTIFY_D_MAX_PENDING=1024`（事件队列容量）、`NOTIFY_D_MAX_CLIENTS=128`、
  `NOTIFY_D_MAX_SUBSCRIPTIONS=512`（订阅注册表容量）、`NOTIFY_D_MAX_CONN=128`
  （并发连接上限）、`NOTIFY_D_MAX_BUFFER=65536`；
- 监听：`NOTIFY_D_DEFAULT_PORT=8084` 仅 Windows TCP；Linux 固定 Unix socket
  `airy_runtime_dir_socket("notify.sock")`；
- 无配置文件、无环境变量开关。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`commons` 基础库、cJSON、线程；Windows
  额外 `ws2_32`；
- 构建产物：可执行文件 `notify_d`。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target notify_d
```

## 测试

CTest 测试（`notify_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `notify_d_test_notify_service` | 订阅注册表 / 事件队列 / 广播 / JSON-RPC 分发 |

```bash
ctest --test-dir build -R "notify_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
