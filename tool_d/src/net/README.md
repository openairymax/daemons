# network — 网络通信抽象层

**模块路径**: `daemons/tool_d/src/net/` · **版本**: 0.1.21

跨平台（Windows Winsock2 / POSIX）的 TCP Socket 封装，附 HTTP/1.1 明文客户端、连接池与 DNS 解析工具。

> 0.1.21 归位：本族自 `commons/utils/network/` 迁入 tool_d。机制核只保留核心内被消费
> 或对外成立契约的能力；本族的生产消费者只有本守护进程的 `builtin_net`（网络类内置工具），
> 技术上属工具形态面私有实现，故随实现落在 tool_d，不再计入机制核账本。

## 概述

- **统一 Socket 抽象**：连接生命周期、收发、超时与非阻塞设置经一套 `network_*` API 屏蔽平台差异；Windows 上由 `network_init()`（或首次 `network_connect()`）惰性完成 `WSAStartup`。
- **HTTP/1.1 客户端**：在已建立连接上构造并发送请求、解析状态码与响应体，`network_http_get` / `network_http_post` 为便捷封装。
- **连接池**：按需惰性创建连接并缓存复用，`network_pool_health_check` 清理失效条目。
- **DNS 与地址工具**：`getaddrinfo` 封装的多地址解析（IPv4/IPv6）、主机可达性探测、本机 IP 查询、地址转字符串。
- **统计与事件**：每连接字节/包/连接/错误/重试计数；连接建立、断开与数据收发时同步触发事件回调。

## 目录结构

```
daemons/tool_d/src/net/
├── network_common.h            # 全部公共类型与 API 声明
├── network_common_internal.h   # 内部结构（connection/pool）与跨文件助手（不安装）
├── network_common.c            # 连接生命周期与收发 IO 域
├── network_http.c              # HTTP 请求构造与响应解析域
├── network_pool.c              # 连接池域
├── network_dns.c               # DNS 解析与地址工具域
└── README.md
```

## 常量

| 名称 | 值 | 说明 |
|---|---|---|
| `NETWORK_DEFAULT_TIMEOUT_MS` | 30000 | 默认连接/读写超时 |
| `NETWORK_DEFAULT_MAX_RETRIES` | 3 | `*_all` / `*_exact` 默认重试次数 |
| `NETWORK_DEFAULT_RETRY_INTERVAL` | 1000 | 默认重试间隔（毫秒） |
| `NETWORK_DEFAULT_BUFFER_SIZE` | 8192 | 默认缓冲尺寸参考 |
| `NETWORK_MAX_POOL_SIZE` | 32 | 连接池上限 |

## 数据结构

### network_config_t — 连接配置

| 字段 | 类型 | 说明 |
|---|---|---|
| `host` / `port` | `const char *` / `int` | 目标主机（IP 或域名）与端口 |
| `timeout_ms` | `int` | 连接时同时应用于读与写的超时 |
| `read_timeout_ms` / `write_timeout_ms` | `int` | 读/写超时；建连阶段取 `timeout_ms`，之后可经 `network_set_rw_timeout` 生效 |
| `max_retries` | `int` | `network_send_all` / `network_receive_exact` 的失败重试上限 |
| `retry_interval_ms` | `int` | 重试间隔配置（当前实现重试间不休眠） |
| `sock_type` | `network_sock_type_t` | `STREAM` / `DGRAM` / `RAW` |
| `af` | `network_af_t` | `AF_UNSPEC` / `AF_INET`(2) / `AF_INET6`(10) |
| `keepalive` | `bool` | 保活配置位（当前实现未设置 `SO_KEEPALIVE`） |
| `nonblocking` | `bool` | 建连前对 socket 置非阻塞标志 |
| `ssl_enable` / `ssl_verify` / `ssl_cert_path` / `ssl_key_path` / `ssl_ca_path` | — | SSL/TLS 配置位；当前实现为明文传输，这些字段不产生效果 |

`network_create_default_config()` 返回：`127.0.0.1:8080`、超时 30000/读写 10000、重试 3×1000ms、`STREAM`/`INET`、其余关闭。

### 其他类型

| 类型 | 说明 |
|---|---|
| `network_connection_t` / `network_pool_t` | 不透明句柄 |
| `network_status_t` | `DISCONNECTED(0)` / `CONNECTING(1)` / `CONNECTED(2)` / `DISCONNECTING(3)` / `ERROR(4)` |
| `network_stats_t` | `bytes_sent`、`bytes_received`、`packets_sent`、`packets_received`、`connect_count`、`error_count`、`retry_count`、`avg_latency_us`（`uint64_t`） |
| `network_event_t` / `network_event_callback_t` | 事件 `CONNECTED(1)` / `DISCONNECTED(2)` / `DATA_RECEIVED(3)` / `DATA_SENT(4)` / `ERROR(5)` / `TIMEOUT(6)`；回调签名 `(conn, event, data, data_len, user_data)` |
| `network_http_request_t` | `method`、`path`、`content_type`、`body`/`body_len`、`headers`/`header_count`、`timeout_ms`、`follow_redirects`、`max_redirects`（重定向字段当前实现不处理） |
| `network_http_response_t` | 出参：`status_code`、`status_text`、`headers`/`header_count`、`body`/`body_len`、`error`、`error_message`、`latency_us`；经 `network_http_response_free` 释放 |
| `network_dns_result_t` | `addresses`（字符串数组）、`count`、`ports`；经 `network_dns_result_free` 释放 |

## 接口

### 连接生命周期与 IO

| 函数 | 语义 |
|---|---|
| `network_init()` / `network_cleanup()` | 子系统初始化/清理（Windows Winsock；POSIX 为空操作） |
| `network_connection_create(config)` / `network_connection_destroy(conn)` | 创建/销毁句柄；`config` 被拷贝入句柄；destroy 时自动断开 |
| `network_connect(conn)` | 建连：host 优先按 IPv4 字面量解析，否则经 `getaddrinfo` 取首地址；仅 `DISCONNECTED`/`ERROR` 态可发起（否则返回 `AIRY_EBUSY`） |
| `network_disconnect(conn)` | 关闭 socket 并置 `DISCONNECTED`；非连接态幂等返回成功 |
| `network_send` / `network_receive` | 单次发送/接收，出参可选返回实际字节数；未连接返回 `AIRY_ENOTCONN`，对端关闭返回 `AIRY_ECONNRESET` |
| `network_send_all` / `network_receive_exact` | 循环发送/收满指定长度，失败按 `max_retries` 重试，最终仍收不满返回 `AIRY_ETIMEDOUT` |
| `network_get_status(conn)` / `network_get_error_message(conn)` | 状态查询；错误消息为空时返回 `"No error"`，NULL 句柄返回固定文案 |
| `network_set_timeout` / `network_set_rw_timeout` | 更新超时并即时施加到已连接 socket |
| `network_get_stats` / `network_reset_stats` | 统计快照/清零 |
| `network_set_event_callback(conn, cb, user_data)` | 注册事件回调（在调用者线程内同步触发） |

### HTTP 客户端

| 函数 | 语义 |
|---|---|
| `network_http_request(conn, req, resp)` | 在已连接句柄上发送 HTTP/1.1 请求（请求行、`Host`、`Content-Type`、`Content-Length`、自定义头），读回响应并解析 |
| `network_http_get(conn, path, resp)` | GET 便捷封装 |
| `network_http_post(conn, path, content_type, body, len, resp)` | POST 便捷封装（`content_type` 为空默认 `application/json`） |
| `network_http_response_free(resp)` | 释放响应内部分配并把结构清零 |

### 连接池

| 函数 | 语义 |
|---|---|
| `network_pool_create(config, pool_size)` | 创建池；`pool_size` 须为 1..32，否则返回 NULL |
| `network_pool_destroy(pool)` | 销毁池并断开其中全部连接 |
| `network_pool_acquire(pool, timeout_ms)` | 返回一个已连接句柄；无现成连接且未达上限时惰性新建并连接；池满或新建失败返回 NULL |
| `network_pool_release(pool, conn)` | 归还入口（当前实现为无操作，连接留在池内继续复用） |
| `network_pool_available` / `network_pool_size` | 可用数（已连接数 + 剩余容量）/ 当前条目数 |
| `network_pool_health_check(pool)` | 销毁 `ERROR`/`DISCONNECTED` 条目（尾部交换压缩），返回健康连接数 |

### DNS 与地址工具

| 函数 | 语义 |
|---|---|
| `network_dns_resolve(hostname, af, result)` | `getaddrinfo` 解析，产出全部地址与端口；失败返回 `AIRY_EIO`/`AIRY_ENOENT` |
| `network_dns_result_free(result)` | 释放解析结果 |
| `network_is_reachable(host, timeout_ms)` | 以默认配置的端口尝试 TCP 建连探测（超时缺省 5000ms） |
| `network_get_local_ip(af, buffer, buffer_len)` | 查询本机出口 IP；失败回退写入 `127.0.0.1` |
| `network_addr_to_string(af, addr, buffer, buffer_len)` | 二进制地址转字符串（支持 v4/v6） |

## 语义与约束

- **明文传输**：当前实现不含 TLS 握手，`ssl_*` 与 `keepalive` 配置位不产生效果；HTTPS 端点不可直连。
- **连接路径为 IPv4**：句柄内部地址结构为 `sockaddr_in`；`NETWORK_AF_INET6` 仅在 DNS 解析与地址工具中有效。
- **HTTP 解析为最小实现**：响应聚合上限 65535 字节；响应头整体存于 `headers[0]`（`header_count` 为 1），不做逐头拆分、不处理 chunked 编码与重定向；状态行不符合 `HTTP/1.x NNN` 形态时 `status_code` 记为 200；`status_text` 与 `latency_us` 不由实现填充。
- **事件覆盖范围**：实现触发 `CONNECTED`、`DISCONNECTED`、`DATA_SENT`、`DATA_RECEIVED`；`ERROR`/`TIMEOUT` 枚举预留、当前无触发点。
- **线程模型**：内部无锁。一个连接/池句柄同一时刻由单一线程使用；统计计数为普通读改写。
- **错误约定**：参数非法返回 `AIRY_EINVAL`，未连接 `AIRY_ENOTCONN`，IO 失败 `AIRY_EIO`，忙 `AIRY_EBUSY`，对端关闭 `AIRY_ECONNRESET`，收不满 `AIRY_ETIMEDOUT`；创建/池类失败经错误栈宏返回 NULL。

## 用法示例

```c
#include "network_common.h"

void fetch_status_path(void)
{
    network_init();

    network_config_t config = network_create_default_config();
    config.host = "internal-service.local";
    config.port = 80;
    config.timeout_ms = 10000;

    network_connection_t *conn = network_connection_create(&config);
    if (conn == NULL) {
        network_cleanup();
        return;
    }

    if (network_connect(conn) == AIRY_SUCCESS) {
        network_http_response_t response = {0};
        if (network_http_get(conn, "/status", &response) == AIRY_SUCCESS &&
            response.status_code == 200) {
            /* response.body / response.body_len 为响应体 */
        }
        network_http_response_free(&response);
        network_disconnect(conn);
    }

    network_connection_destroy(conn);
    network_cleanup();
}
```

## 构建与依赖

四个 `.c` 随 tool_d 服务静态库编译（`CMakeLists.txt` 的 `TOOL_SERVICE_SOURCES`），头文件只在本守护进程与测试内可见，不再安装到 SDK include 树；`network_common_internal.h` 仍按约定不对外。

| 依赖 | 用途 |
|---|---|
| commons `utils/memory` | 句柄与响应内部分配（`AIRY_CALLOC`、`AIRY_STRDUP`、`AIRY_FREE`） |
| commons `utils/error` | 错误码与错误栈宏 |
| commons `utils/types`（`types.h`） | 基础类型 |
| commons `utils/compat`（`atomic_compat.h`） | Winsock 一次性初始化的原子标志 |
| 平台网络栈 | POSIX socket 或 Winsock2（Windows 经 `#pragma comment` 链接 `ws2_32.lib`） |

单元测试见 `daemons/tool_d/tests/test_network.c`（离线部分：默认配置、句柄生命周期、NULL 处理、枚举取值、HTTP 结构释放、池参数校验与计数）。

---

*SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0*
*Copyright (c) 2025-2026 SPHARX Ltd. 及贡献者，详见 [LICENSE](../../LICENSE)。*
