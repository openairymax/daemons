# channel_d — IPC 通道守护进程

> **模块路径**：`agentrt/daemons/channel_d/` · **可执行文件 / CMake 目标**：`channel_d` · **RPC 命名空间**：`channel.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`channel_d` 提供 AgentRT 的统一 IPC 通道服务：在 SOCKET / SHM / PIPE 三类通道上完成
创建、打开、关闭、数据收发与连通性探测，供各守护进程与外部组件按通道标识传输消息。
调用方通过 `gateway_d` 的 `channel.*` 命名空间访问，也可直连本进程端点。

- 端点：POSIX Unix socket `<runtime-dir>/channel.sock`（`$AIRY_HOME/run/channel.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8094`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8094`（默认只监听 socket）。
- 通道端点目录（默认 `$AIRY_TMP_DIR/channels`）由进程启动时幂等逐级创建。

## 能力

- **三类通道** — SOCKET（`AF_UNIX` SOCK_STREAM，非阻塞，backlog 128）、SHM
  （`shm_open` + `mmap` + 标志位与内存栅栏）、PIPE（`mkfifo` 命名管道）。
- **通道表管理** — 单表上限 256 个通道，全部操作互斥串行；重名打开报错。
- **数据收发** — 长度前缀帧格式传输，累计收发计数进入 `get_stats`。
- **连通性探测** — `ping` 带通道标识时返回真实往返延迟（毫秒）。
- **空闲超时** — 默认 30 s 空闲超时，可配置。

## 架构

```
gateway_d ──(channel.* JSON-RPC)──▶ channel_d
                                      │ channel_service（SOCKET / SHM / PIPE 通道表）
                                      │  ├─ channel_service.c  生命周期 / 打开关闭 / 查询
                                      │  └─ channel_io.c       收发与连通性探测
                                      ▼
                              各守护进程间 IPC 传输
```

- 事件驱动：`daemon_event_driver`（线程池 4~8，队列 256，`max_events=64`）接收
  JSON-RPC 请求并派发到已注册方法。
- 传输帧格式：SOCKET / PIPE 为 `[4 字节网络序长度][数据]`；SHM 写共享内存后置标志位
  （`memory_order_seq_cst` 栅栏），接收侧读后清标志。

## 通道类型与状态

| 通道类型 | 枚举值 | 说明 |
|----------|--------|------|
| SOCKET | `CHANNEL_TYPE_SOCKET` (0) | `AF_UNIX` SOCK_STREAM，非阻塞，backlog 128 |
| SHM | `CHANNEL_TYPE_SHM` (1) | `shm_open` + `mmap`，帧格式 `[4B 长度][4B 标志][数据]` |
| PIPE | `CHANNEL_TYPE_PIPE` (2) | `mkfifo` 命名管道，非阻塞读写 |

| 通道状态 | 枚举值 |
|----------|--------|
| CLOSED | 0 |
| OPEN | 1 |
| ERROR | 2 |
| DRAINING | 3 |

## JSON-RPC 接口

共 9 个方法，经 `method_dispatcher_register` 注册（方法名不含命名空间前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `open` | `{id, name, type?: int(0~2)}` | `{status: "opened"}` | 打开通道，`type` 缺省为 SOCKET；重名报错 |
| `close` | `{id}` | `{status: "closed"}` | 关闭并销毁通道 |
| `send` | `{id, data: string}` | `{status: "sent"}` | 向通道发送数据（非字符串参数直接拒绝） |
| `list` | `{}` | `{channels: [{id, name, type, status, sent, recv}...], ...}` | 列出全部通道 |
| `ping` | `{id?: string\|number}` | 无 `id`：`{status: "ok"\|"degraded"}`；有 `id`：`{status: "ok", channel_id, latency_ms}` | 服务存活探测或通道往返延迟测量 |
| `health` | `{}` | `{healthy: bool}` | 健康状态 |
| `health_check` | `{}` | `{healthy: bool}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, channels, messages_sent, messages_received}` | 通道数与累计收发计数 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`channel.open`），由 `gateway_d` 剥离前缀后转发。

参数校验失败（缺 `id` / `data`、类型非法）返回 JSON-RPC `-32602`，其余服务错误返回 `-32603`。

## 服务 API

`include/channel_service.h`（安装至 `include/agentrt/daemons/channel_d/`）：

- 生命周期：`channel_service_create / destroy / start / stop`
- 通道操作：`channel_service_open / close / send / receive`
- 查询：`channel_service_list / get_info`
- 事件：`channel_service_set_callback`（`channel_message_cb_t`）
- 探测：`channel_service_ping`（返回 `latency_ms`）、`channel_service_is_healthy`

## 配置

命令行参数：

| 参数 | 说明 |
|------|------|
| `--manager <config>` / `-c <config>` | 为兼容统一启动参数而接受，本进程不加载该文件 |
| `-s <dir>` | 覆盖通道端点目录 |
| `-n <n>` | 覆盖最大通道数 |
| `--tcp` | 以 TCP 回环模式监听 |
| `-h` / `--help` | 帮助 |

内建默认配置 `CHANNEL_CONFIG_DEFAULTS`：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `max_channels` | 256 | 通道表上限 |
| `default_buffer_size` | 65536 | 默认缓冲区 / 消息上限 |
| `socket_backlog` | 128 | SOCKET 监听 backlog |
| `socket_dir` | `$AIRY_TMP_DIR/channels` | SOCKET / PIPE 端点目录 |
| `shm_prefix` | `/airy_ch_` | SHM 名称前缀 |
| `idle_timeout_ms` | 30000 | 空闲超时 |

环境变量：`AIRY_CHANNEL_SOCK` 用于在网关侧覆盖本进程端点。

## 用法

```bash
airymaxrt logs channel_d       # 运行态日志
<build-dir>/bin/channel_d      # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target channel_d
ctest --test-dir ../daemons-build -R channel_e2e --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、共享内存与 socket 封装 |
| [corekern](https://atomgit.com/openairymax/atoms) | `airy_init()` 核心引导 |
| [gateway_d](../gateway_d/README.md) | 上游：`channel.*` 命名空间转发方 |

## 关系

`channel_d` 管理的是**通道资源本身**（建立、收发、销毁）；进程间的请求-响应调用仍走
各自的 JSON-RPC 端点，由 `gateway_d` 统一路由。`monit_d` 可观测通道数量与收发计数。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
