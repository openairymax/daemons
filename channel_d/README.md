# Channel Daemon — IPC 通道守护进程

> **模块路径**: `agentrt/daemons/channel_d/`

## 定位

`channel_d` 是 AgentRT 的 IPC 通道管理守护进程（IMP-08：统一通道服务），为各守护进程提供
SOCKET / SHM / PIPE 三类 IPC 通道的创建、打开、关闭、数据收发与健康监控。它向上层暴露
`channel.*` JSON-RPC 方法族（L2 服务协议，`02-l2-service-protocol.md` §6.1），是
AgentRT IPC 基础设施的组成部分，常由 gateway_d 通过 `channel.*` 命名空间转发调用。

## 架构

```
gateway_d ──(channel.* JSON-RPC)──▶ channel_d
                                      │ channel_service（SOCKET/SHM/PIPE 通道表）
                                      │  ├─ SOCKET: AF_UNIX SOCK_STREAM（backlog=128，非阻塞）
                                      │  ├─ SHM:    shm_open + mmap + 标志位 + 内存栅栏
                                      │  └─ PIPE:   mkfifo 命名管道
                                      ▼
                              各守护进程间 IPC 传输
```

- 单文件 service（`src/channel_service.c`）持有通道表（上限 `CHANNEL_MAX_CHANNELS=256`），
  全部操作经互斥锁串行化。
- 事件驱动模式：`daemon_event_driver`（线程池 4~8，队列 256，`max_events=64`）接收
  JSON-RPC 请求并派发到已注册方法。
- 通道端点目录（默认 `$AIRY_TMP_DIR/channels`）由 daemon 启动时幂等逐级 `mkdir` 自管，
  不依赖外部预创建（避免 bind 因 ENOENT 返回 -32603）。

### 通道类型与状态

| 通道类型 | 枚举值 | 说明 |
|----------|--------|------|
| SOCKET | `CHANNEL_TYPE_SOCKET` (0) | AF_UNIX SOCK_STREAM，非阻塞，backlog=128 |
| SHM | `CHANNEL_TYPE_SHM` (1) | `shm_open` + `mmap`，帧格式 `[4B 长度][4B 标志][数据]`，`atomic_thread_fence` 同步 |
| PIPE | `CHANNEL_TYPE_PIPE` (2) | `mkfifo` 命名管道，非阻塞读写 |

| 通道状态 | 枚举值 |
|----------|--------|
| CLOSED | 0 |
| OPEN | 1 |
| ERROR | 2 |
| DRAINING | 3 |

传输帧格式：SOCKET/PIPE 为 `[4 字节网络序长度][数据]`；SHM 通过写共享内存 + 置标志位
（`memory_order_seq_cst` 栅栏）传递消息，接收侧读后清标志。

## JSON-RPC 接口表

监听端点：Unix socket `$AIRY_RUNTIME_DIR/channel.sock`（`--tcp` 或 Windows 下为
TCP `127.0.0.1:8094`，Windows pipe `\\.\pipe\airy_channel`）。以下方法由
`method_dispatcher_register` 实际注册（main.c），共 9 个：

| 方法 | 参数 | 返回要点 | 说明 |
|------|------|----------|------|
| `ping` | `id`（可选，字符串或数字） | 无 `id`：`{"status":"ok"\|"degraded"}`；有 `id`：`{"status":"ok","channel_id","latency_ms"}` | 健康探测或通道往返延迟测量（SOCKET 实际 connect，SHM/PIPE 探测端点） |
| `list` | 无 | `{"channels":[{"id","name","type","status","sent","recv"},...]}` | 列出全部通道 |
| `open` | `id`, `name`, `type`（0~2，缺省 SOCKET） | `{"status":"opened"}` | 打开通道；重名返回错误 |
| `close` | `id` | `{"status":"closed"}` | 关闭并销毁通道 |
| `send` | `id`, `data`（必须为字符串，否则 fail-closed 报错） | `{"status":"sent"}` | 向通道发送数据 |
| `health` | 无 | `{"healthy":true\|false}` | 健康状态 |
| `health_check` | 无 | `{"healthy":true\|false}` | L2 标准方法，同 `health` |
| `shutdown` | 无 | — | L2 标准方法，触发优雅退出 |
| `get_stats` | 无 | `{"daemon":"channel_d","channels":N,"messages_sent":N,"messages_received":N}` | 通道数 + 累计收发消息数（真实统计） |

错误码约定：参数校验失败（缺 id/data、类型非法）映射 JSON-RPC `-32602`（Invalid params）；
其余服务错误映射 `-32603`（Internal error）。

### 服务 API（channel_service_*）

`include/channel_service.h`（安装至 `include/agentrt/daemons/channel_d`）：

- 生命周期：`channel_service_create/destroy/start/stop`
- 通道操作：`channel_service_open/close/send/receive`
- 查询：`channel_service_list/get_info`
- 事件：`channel_service_set_callback`（`channel_message_cb_t`）
- 探测：`channel_service_ping`（返回 `latency_ms`）、`channel_service_is_healthy`

## 配置

- 默认配置 `CHANNEL_CONFIG_DEFAULTS`：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `max_channels` | 256 | 最大通道数 |
| `default_buffer_size` | 65536 | 默认缓冲区/消息上限 |
| `socket_backlog` | 128 | SOCKET 监听 backlog |
| `socket_dir` | `$AIRY_TMP_DIR/channels` | SOCKET/PIPE 端点目录（自管 mkdir） |
| `shm_prefix` | `/airy_ch_` | SHM 名称前缀 |
| `idle_timeout_ms` | 30000 | 空闲超时 |

- 命令行参数：

| 参数 | 说明 |
|------|------|
| `--manager <config>` / `-c <config>` | 兼容 bootstrap 统一启动参数（忽略） |
| `-s <dir>` | 覆盖 socket 目录 |
| `-n <n>` | 覆盖最大通道数 |
| `--tcp` | 以 TCP 回环模式监听（Windows 强制） |
| `-h` / `--help` | 帮助 |

## 依赖与构建

- 依赖：`svc_common`（daemon 公共库）、`airy_common`（commons 统一基础库）、
  `Threads::Threads`、`airy_platform_libs`。
- Linux 构建时定义 `AIRY_CONFIG_DIR=/etc/agentrt`、`AIRY_LOG_DIR=/var/log/agentrt`。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target channel_d
```

## 测试

- `tests/test_channel_e2e.c`（`channel_e2e`）：E2E 覆盖生命周期、健康检查、open/close/list、
  send/receive 与延迟、回调机制、错误处理；支持 cmocka 或内置 stub 两种构建路径。
- 运行：

```bash
ctest --test-dir build -R channel_e2e -V
```
