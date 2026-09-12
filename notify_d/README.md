# notify_d — 多协议通知守护进程

> **模块路径**：`agentrt/daemons/notify_d/` · **可执行文件 / CMake 目标**：`notify_d` · **RPC 命名空间**：`notify.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`notify_d` 是 AgentRT 的事件通知枢纽：把运行时内部产生的事件，以**主题（topic）**
为单位广播给外部订阅者。它对内提供 JSON-RPC 2.0 接口用于发布与订阅管理，对外
同时支持三种投递协议——**WebSocket**、**SSE** 与**原始 Unix socket JSON**。

- 端点：POSIX Unix socket `<runtime-dir>/notify.sock`（`$AIRY_HOME/run/notify.sock`）；
  Windows 本机 TCP 回环 `127.0.0.1:8084`。
- 与 `channel_d` 的区别：`channel_d` 是数据面的**传输通道**（对外协议适配），
  `notify_d` 是控制面的**事件广播**；两者的「通道 / topic」概念互不相干。

## 能力

- **三种订阅协议** — 同一进程内并存 WebSocket 客户端、SSE 客户端与普通
  JSON-RPC 客户端，按连接协商出的协议分别编码投递。
- **主题订阅注册表** — `(topic, client_id)` 二元组订阅，注册与注销均幂等。
- **异步事件队列** — 环形队列（容量 1024）+ 后台广播线程，发布方不被慢消费者阻塞。
- **双路匹配投递** — 事件同时匹配「连接握手期声明的 `X-Topic`」与「订阅注册表
  中登记的 `(topic, client_id)`」。
- **零外部加密依赖** — WebSocket 握手所需的 SHA-1 与 Base64 在 `main.c` 内自带实现。

## 架构

```
事件源（各守护进程 / notify.publish）
        │
        ▼
  事件队列（ring buffer，1024；后台线程消费）
        │
        ▼
  广播引擎（topic 匹配 ∪ 订阅注册表匹配）
        ├── WebSocket 客户端（文本帧 0x81）
        ├── SSE 客户端（event: / data: 帧）
        └── Unix socket 客户端（原始 JSON）
```

- `src/main.c` — 监听与 accept 循环、每连接独立线程（并发上限 128，超限直接关闭
  新连接）、握手与帧编码、服务端投递；
- `src/notify_service.c` — 订阅注册表、事件入队与广播、JSON-RPC 分发
  （`notify_d_dispatch_jsonrpc`）；
- 健康判定：队列已满，或（已通知数 > 10 且 错误数 > 已通知数的一半）时判为不健康。

## JSON-RPC 接口

共 8 个方法，由 `notify_d_dispatch_jsonrpc` 按方法名分发（`method` 不带 `notify.` 前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `publish` | `message` 或 `payload`(必填)、`topic`(默认 `"default"`)、`event`(默认 `"message"`) | `{queued, topic, event, pending, subscribers}` | 事件入队，交由广播线程投递 |
| `subscribe` | `topic`(必填)、`client_id`(必填) | `{status:"subscribed", topic, client_id, subscribers}` | 登记订阅（幂等） |
| `unsubscribe` | `topic`(必填)、`client_id`(必填) | `{status:"unsubscribed", ...}` | 撤销订阅（幂等） |
| `list` | `{}` | `{clients, subscriptions, topics:[{topic, subscribers, active_clients}]}` | 列出在线客户端与各主题订阅数 |
| `health` | `{}` | `{status, service, queue_pending, queue_capacity, queue_occupancy, consumer_running, active_clients, subscriptions, notified, errors, uptime_s}` | 队列与广播引擎健康状态 |
| `get_stats` | `{}` | `{daemon, uptime_s, notified, errors, clients, pending}` | 运行统计 |
| `health_check` | `{}` | `{status:"ok", service, uptime_s, timestamp}` | 存活探针 |
| `shutdown` | `{}` | `{status:"shutting_down"}` | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`notify.publish`），由 `gateway_d` 剥离前缀后转发。

## 配置

`notify_d` **没有配置文件，也不解析命令行参数**（`main()` 不使用 `argc/argv`），
全部行为由编译期常量决定：

| 常量 | 值 | 说明 |
|------|-----|------|
| `NOTIFY_D_DEFAULT_PORT` | 8084 | Windows TCP 回环端口 |
| `NOTIFY_D_DEFAULT_SOCKET` | `airy_runtime_dir_socket("notify.sock")` | POSIX 端点 |
| `NOTIFY_D_MAX_PENDING` | 1024 | 事件队列容量 |
| `NOTIFY_D_MAX_CLIENTS` | 128 | 客户端表容量 |
| `NOTIFY_D_MAX_SUBSCRIPTIONS` | 512 | 订阅注册表容量 |
| `NOTIFY_D_MAX_CONN` | 128 | 并发连接线程上限 |
| `NOTIFY_D_MAX_BUFFER` | 65536 | 单条报文缓冲上限 |

连接方可在握手请求中携带：`X-Client-Id`（声明客户端身份，用于订阅注册表匹配）、
`X-Topic`（声明该连接订阅的主题，用于广播直投）。

网关侧可用 `AIRY_NOTIFY_SOCK` 覆盖 `notify_d` 的端点。

## 用法

```bash
airymaxrt logs notify_d        # 运行态日志
<build-dir>/bin/notify_d       # 手工启动单个进程（监听运行目录）
```

订阅端直连验证（POSIX）：

```bash
printf '{"jsonrpc":"2.0","id":1,"method":"health_check","params":{}}' \
  | socat - UNIX-CONNECT:"${AIRY_HOME:-$HOME/.airymaxrt}/run/notify.sock"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target notify_d
ctest --test-dir ../daemons-build -R notify_d_ --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

| CTest 用例 | 覆盖点 |
|-----------|--------|
| `notify_d_test_notify_service` | 订阅注册表、事件队列、广播匹配、JSON-RPC 分发 |

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、日志、服务注册 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、socket / 线程抽象、cJSON |
| [gateway_d](../gateway_d/README.md) | 上游：`notify.*` 命名空间转发方 |

## 关系

- `notify_d` 只负责**广播**，不生产事件；各守护进程通过 `notify.publish` 或经
  `gateway_d` 上报事件。
- 与 `monit_d` 分工不同：`monit_d` 面向运维的可观测数据拉取，`notify_d` 面向
  业务消费者的事件推送。
- 与 `channel_d` 分工不同：`channel_d` 承载对外协议的数据面通道，`notify_d`
  的 topic 是纯逻辑订阅键。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
