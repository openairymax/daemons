# monit_d — 监控告警守护进程

> **模块路径**：`agentrt/daemons/monit_d/` · **可执行文件 / CMake 目标**：`monit_d` · **RPC 命名空间**：`monit.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`monit_d` 是 AgentRT 的可观测性守护进程：运行态指标采集与查询、健康检查、告警触发与
解决、监控报告生成、分布式追踪，并把指标以 Prometheus 文本格式导出。除 JSON-RPC 之外，
同一监听端点直接识别 `GET /metrics` 请求，供 Prometheus 抓取。

- 端点：POSIX Unix socket `<runtime-dir>/monit.sock`（`$AIRY_HOME/run/monit.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:9090`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `9090`（默认只监听 socket）。
- 另有观测域独立 HTTP 抓取端点 `:9091/metrics`，端口被占用时自动降级，不影响启动。

## 能力

- **指标域** — 记录 / 查询命名指标，心跳自动落 `heartbeat` counter，30s 周期上报统计。
- **告警域** — 触发告警、按 `alert_id` 解决告警、列出活动与已解决告警。
- **健康与报告** — 单服务健康检查、整体监控报告生成、`get_stats` 运行态自计数。
- **Prometheus 导出** — 14 个必需指标 + 2 个抓取统计 gauge，与 JSON-RPC 共用监听端点。
- **观测域（`observe_*`）** — 扁平参数风格的动态指标表（最多 256 条），并额外暴露独立
  HTTP `/metrics` 服务。
- **系统信息域（`info_*`）** — 后台线程每 5s 采集一次 CPU / 内存 / 磁盘快照，保留 64 深度
  环形历史，可查询系统画像与硬件（含加速器）信息。

## 架构

```
客户端 (JSON-RPC 2.0) ─┐
Prometheus scraper     ├─▶ main.c（事件驱动，先判 GET /metrics 再按 JSON-RPC 分发）
(GET /metrics 同端点) ─┘                          │
                    ┌─────────────────────────────┼───────────────────────────┐
                    ▼                             ▼                           ▼
      monitor_service（metrics / alert /   observe_rpc                info_rpc
        tracing / logging / report）       （动态指标表 + :9091 HTTP） （5s 采集线程 + 环形历史）
                    │
                    ▼
      prometheus_exporter（指标注册与文本导出）
```

- 事件驱动模型：`daemon_event_driver`，线程池 2~4、队列 128、最大事件 64；
- 请求分流顺序：`GET /metrics` 命中即返回 Prometheus 文本（`text/plain; version=0.0.4`），
  否则按 JSON-RPC 2.0 分发；
- `observe_rpc` / `info_rpc` 与 `monitor_service` 同为内建模块，初始化失败只降级对应方法，
  不阻断守护进程启动。

## JSON-RPC 接口

共 19 个方法，均由 `method_dispatcher_register` 注册，分布在三个源文件中。

**核心监控域（`src/main.c`，12 个）**

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `record_metric` | `{metric:{name, description?, type?, value?}}` | `{status:"recorded", metric_name}` | 记录嵌套对象形式的指标 |
| `get_metrics` | `{metric_name?}` | `[{name, description, type, value, timestamp}]` | 查询指标，可按名称过滤 |
| `trigger_alert` | `{alert:{alert_id, message, level?, service_name?, resource_id?}}` | `{status:"triggered", alert_id}` | 触发告警 |
| `get_alerts` | `{}` | `[{alert_id, message, level, service_name, is_resolved, timestamp}]` | 列出告警 |
| `alert_resolve` | `{alert_id}` | — | 解决告警；`alert_id` 不存在返回方法未找到 |
| `heartbeat` | `{description?, value?}` | `{received:true, service, timestamp}` | 记录心跳（`value` 默认 `1.0`） |
| `health_check` | `{service_name?}` | `{service_name, healthy, status_message, timestamp}` | 服务健康检查（`service_name` 默认 `unknown`） |
| `generate_report` | `{}` | `{report, generated_at}` | 生成监控报告 |
| `metrics` | 同 `get_metrics` | 同 `get_metrics` | `get_metrics` 别名 |
| `alert_raise` | 同 `trigger_alert` | 同 `trigger_alert` | `trigger_alert` 别名 |
| `get_stats` | `{}` | `{daemon, uptime_s, metrics, alerts, alerts_resolved}` | 运行态自计数 |
| `shutdown` | `{}` | — | 优雅退出 |

**观测域（`src/observe_rpc.c`，3 个）**

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `observe_record_metric` | `{name, value, type?: "gauge"\|"counter", unit?}` | `{status:"recorded", name, value, type, unit?}` | 记录动态指标（`name`、`value` 必填） |
| `observe_query_metrics` | `{name?}` | `{count, metrics:[{name, value, type, unit}]}` | 查询动态指标 |
| `observe_get_metrics` | 同 `observe_query_metrics` | 同 `observe_query_metrics` | `observe_query_metrics` 别名 |

**系统信息域（`src/info_rpc.c`，4 个）**

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `info_system` | `{}` | `{service, platform, hostname, kernel_version, system:{…}}` | 系统标识 + 最新一次采集快照 |
| `info_history` | `{N?\|n?\|count?\|limit?}` 或 `[N]` | `[{timestamp, cpu_cores, cpu_usage_pct, total_memory_kb, free_memory_kb, used_memory_kb, memory_usage_pct, disk_total_kb, disk_free_kb, disk_used_kb, disk_usage_pct, uptime_sec}]` | 环形历史，缺省返回全部 64 条 |
| `info_health` | `{}` | `{status, service, collecting, running, last_collect_time, staleness_sec, uptime_s, timestamp}` | 采集线程健康度；快照滞后超过 3 个采集周期判 `degraded` |
| `info_hardware` | `{}` | `{cpu_count, mem_total_kib, mem_avail_kib, profile, accel_present, accel_count, accel_model}` 或 `{status:"unavailable"}` | 硬件画像，`profile` 为 `minimal` / `full` |

`observe_*` 与 `info_*` 保留各自自带的前缀，用于与 `monit` 原生方法区分（例如
`health_check` 走核心监控域，`info_health` 走系统信息域）。外部调用方使用带命名空间前缀的
形式（`monit.record_metric`），由 `gateway_d` 剥离前缀后转发。

## Prometheus 必需指标

`src/prometheus_exporter.c` 注册 14 个必需指标（1–10 核心可观测性、11–14 内存可观测性），
外加 2 个抓取统计 gauge：

| # | 指标 | 类型 | labels |
|---|------|------|--------|
| 1 | `airy_cognition_latency_ms` | histogram | agent_id |
| 2 | `airy_llm_request_duration_ms` | histogram | provider,model |
| 3 | `airy_llm_cost_usd_total` | counter | provider |
| 4 | `airy_tool_call_total` | counter | tool_name,status |
| 5 | `airy_memory_operations_total` | counter | layer,operation |
| 6 | `airy_hook_execution_ms` | histogram | hook_name,event |
| 7 | `airy_connection_health` | gauge | connection_id |
| 8 | `airy_plugin_lifecycle_total` | counter | plugin_name,event |
| 9 | `airy_llm_retry_total` | counter | provider,error_category |
| 10 | `airy_config_reload_total` | counter | status |
| 11 | `airy_memory_rss_bytes` | gauge | — |
| 12 | `airy_memory_heap_bytes` | gauge | — |
| 13 | `airy_memory_pool_utilization` | gauge | — |
| 14 | `airy_oom_events_total` | counter | level |
| + | `airy_monit_scrape_count` / `airy_monit_scrape_errors` | gauge | — |

必需指标注册失败只记警告，不阻止启动。观测域另有自监控指标
`airy_observe_requests_total`、`airy_observe_errors_total`、`airy_observe_http_requests_total`、
`airy_observe_metrics_count`、`airy_observe_uptime_seconds`。

## 配置

监控参数为进程内建默认值，写在 `src/main.c` 中：

| 字段 | 默认 |
|------|------|
| `metrics_collection_interval_ms` | 5000 |
| `health_check_interval_ms` | 10000 |
| `log_flush_interval_ms` | 30000 |
| `alert_check_interval_ms` | 5000 |
| `log_file_path` | `monitor.log` |
| `metrics_storage_path` | `metrics` |
| `enable_tracing` / `enable_alerting` | `true` |

命令行选项与其他守护进程一致：`--manager <config>` 指定配置路径、`--tcp` 切换到
TCP 监听、`--help` 打印用法。**当前版本 `--manager` 给出的路径只记录到启动日志，不会加载**，
监控参数一律取上表内建值。

| 环境变量 | 说明 |
|----------|------|
| `AIRY_MONIT_SOCK` | 网关侧覆盖 monit_d 端点 |
| `AIRY_HOME` / `AIRY_RUNTIME_DIR` | 运行目录，决定 `monit.sock` 落位 |

## 用法

```bash
airymaxrt logs monit_d        # 运行态日志
<build-dir>/bin/monit_d       # 手工启动单个进程（监听运行目录）
```

直连端点验证（POSIX）：

```bash
SOCK="${AIRY_HOME:-$HOME/.airymaxrt}/run/monit.sock"
printf '%s' '{"jsonrpc":"2.0","id":1,"method":"get_stats","params":{}}' | socat - UNIX-CONNECT:"$SOCK"
printf 'GET /metrics HTTP/1.1\r\nHost: monit\r\n\r\n' | socat - UNIX-CONNECT:"$SOCK"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target monit_d
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

`BUILD_TESTS=ON` 时注册 5 个 CTest 用例：

| 用例 | 覆盖点 |
|------|--------|
| `monit_d_test_metrics` | 指标采集与查询 |
| `monit_d_test_alert` | 告警触发与解决 |
| `monit_d_test_tracing` | 分布式追踪 |
| `monit_d_observe_rpc` | 观测域 RPC 与 HTTP 抓取导出 |
| `monit_d_info_rpc` | 系统信息快照、历史与硬件画像 |

```bash
ctest --test-dir ../daemons-build -R monit_d --output-on-failure
```

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、同步、cJSON 封装 |
| [atoms](https://atomgit.com/openairymax/atoms) | `corekern`（`airy_init()`）、syscall 层 |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 能力层（经 `svc_common` 传递） |

服务代码抽为静态库 `airy_monit_service`，被可执行文件与单元测试共用；
Windows 额外链接 `ws2_32`、`bcrypt`。

## 关系

`monit_d` 关注**运行时自身**的指标、健康与告警。外部事件通知由
[`notify_d`](../notify_d/README.md)（发布 / 订阅）负责，跨进程连接与信道管理由
[`channel_d`](../channel_d/README.md) 负责；三者互不重叠。其他守护进程通过
`monit.*` 命名空间经 `gateway_d` 上报指标，Prometheus 则直接抓取 `/metrics`。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
