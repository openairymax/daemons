# monit_d — 监控告警守护进程（monit.* 命名空间）

> **模块路径**: `agentrt/daemons/monit_d/`
> **命名空间**: `monit.*`（gateway 转发时剥离前缀，方法名不带 `monit.`）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/monit.sock`（TCP 9090 可选，Windows 命名管道 `\\.\pipe\airy_monit`）

## 定位

`monit_d` 是 AgentRT 的监控告警守护进程，负责运行时指标采集与查询、健康检查、
告警触发/解决、监控报告生成与分布式追踪，并提供内建 Prometheus `/metrics`
抓取端点（C-L10），是 AgentRT 可观测性的核心组件。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket / TCP) ──┐
Prometheus scraper (GET /metrics 同一 socket)  ├→ main.c（事件驱动 handle_client 分流）
                                               ↓
                              monitor_service（metrics / alert / tracing / logging）
                                               ↓
                              prometheus_exporter（unified_metrics 注册 + 导出）
```

- 事件驱动模型：`daemon_event_driver`，线程池 2~4、队列 128；
- **Prometheus 端点与 JSON-RPC 共用同一监听 socket**（Linux 默认 Unix socket，
  TCP 模式为 9090）：`handle_client` 先尝试 `prometheus_exporter_handle_http`
  识别 `GET /metrics`，命中则返回 Prometheus 文本格式（`text/plain; version=0.0.4`），
  否则按 JSON-RPC 2.0 分发；无独立 HTTP 端口；
- 30s 定时器上报 scrape 统计并回写 `airy_monit_scrape_count` /
  `airy_monit_scrape_errors` gauge；
- 必需指标注册失败仅告警，不阻止启动。

## JSON-RPC 接口

以下方法表以 `main.c` 中 `method_dispatcher_register` 的真实注册为准（共 12 个方法）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `record_metric` | `metric`(对象，必填；`name` 必填，`description`/`type`/`value` 可选) | 记录指标，返回 `{status:"recorded",metric_name}` |
| `get_metrics` | `metric_name`(可选过滤) | 查询指标数组 `[{name,description,type,value,timestamp}]` |
| `trigger_alert` | `alert`(对象，必填；`alert_id`/`message`/`level`/`service_name`/`resource_id`) | 触发告警，返回 `{status:"triggered",alert_id}` |
| `get_alerts` | — | 告警列表 `[{alert_id,message,level,service_name,is_resolved,timestamp}]` |
| `alert_resolve` | `alert_id`(必填) | 解决告警；不存在返回 `JSONRPC_METHOD_NOT_FOUND` |
| `heartbeat` | `description`(可选)、`value`(默认 1.0) | 记录 `heartbeat` counter 指标，返回 `{received:true,service,timestamp}` |
| `health_check` | `service_name`(默认 "unknown") | `{service_name,healthy,status_message,timestamp}` |
| `generate_report` | — | 生成监控报告，返回 `{report,generated_at}` |
| `metrics` | 同 `get_metrics` | `get_metrics` 的别名（L2 协议标准方法） |
| `alert_raise` | 同 `trigger_alert` | `trigger_alert` 的别名（L2 协议标准方法） |
| `get_stats` | — | `{daemon:"monit_d",uptime_s,metrics,alerts,alerts_resolved}` |
| `shutdown` | — | 优雅关闭（L2 §6.1） |

## Prometheus 必需指标

`prometheus_exporter.c` 注册的必需指标（14 个：1-10 核心可观测性 + 11-14 内存可观测性，
另加 2 个 scrape 统计 gauge）：

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

## 配置

- `monitor_config_t` 默认值（`main.c` 内建）：
  `metrics_collection_interval_ms=5000`、`health_check_interval_ms=10000`、
  `log_flush_interval_ms=30000`、`alert_check_interval_ms=5000`、
  `log_file_path="monitor.log"`、`metrics_storage_path="metrics"`、
  `enable_tracing=true`、`enable_alerting=true`；
- `config_path` 默认 `agentrt/manager/service/monit_d/monit.yaml`。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`commons` 基础库、`unified_metrics`
  （Prometheus 导出）、cJSON；Windows 额外 `ws2_32`；
- 构建产物：可执行文件 `monit_d`。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target monit_d
```

## 测试

CTest 测试（`monit_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `monit_d_test_metrics` | 指标采集 |
| `monit_d_test_alert` | 告警管理 |
| `monit_d_test_tracing` | 分布式追踪 |

```bash
ctest --test-dir build -R "monit_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
