# observe_d — 可观测性守护进程（observe.* 命名空间）

> **模块路径**: `agentrt/daemons/observe_d/`
> **命名空间**: `observe.*`（方法名不带前缀）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/observe.sock`（Linux）；Windows 走 TCP `127.0.0.1:8091`
> **Prometheus 端点**: TCP `:9091/metrics`（仅非 Windows）

## 定位

`observe_d` 是 AgentRT 的可观测性守护进程，提供 Gauge/Counter 两类指标的记录与
查询，并以 Prometheus exposition 格式暴露 `/metrics`，供 Prometheus 等外部监控
系统抓取，是 AgentRT 与外部监控集成的桥梁。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket) ──┐
Prometheus scraper (TCP 9091 /metrics)   ├→ main.c（单文件：RPC 分发 + HTTP 线程）
                                         ↓
                metrics 数组（上限 256，锁保护）
```

- 网络层：自定义 accept 循环（每连接独立线程，并发上限 128）+ 独立 HTTP 线程
  （`observe_d_http_loop`，backlog 16）；
- 指标行为：Counter 累加、Gauge 覆盖；`find_or_create_metric` 自动创建不存在指标；
- 初始化内置 5 个指标：`airy_observe_requests_total`、`airy_observe_errors_total`、
  `airy_observe_http_requests_total`（counter），`airy_observe_metrics_count`、
  `airy_observe_uptime_seconds`（gauge）；
- JSON-RPC 分发在 `main.c` 内手写（非 method_dispatcher）：先解析 JSON 匹配
  `shutdown` / `get_stats` / `health_check` / `record_metric` /
  `query_metrics` / `get_metrics`，未命中则按普通请求处理并回写状态 JSON；
- `main()` 不解析命令行参数，不接受 `--config`；
- 信号：SIGINT/SIGTERM 优雅关闭，SIGPIPE 忽略。

## JSON-RPC 接口

以下方法表以 `main.c` 的真实分发逻辑为准（共 6 个方法，`get_metrics` 为
`query_metrics` 别名）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `record_metric` | `name`(string，必填)、`value`(number，必填)、`type`(string：`gauge`/`counter`，默认 `gauge`)、`unit`(string，可选) | 记录/更新指标，返回 `{status:"recorded",name,value,type,unit?}` |
| `query_metrics` | `name`(可选过滤) | 查询指标，返回 `{count,metrics:[{name,value,type,unit}]}` |
| `get_metrics` | 同 `query_metrics` | `query_metrics` 的别名 |
| `health_check` | — | `{status:"ok",service:"observe_d",uptime_s,timestamp}` |
| `get_stats` | — | `{daemon:"observe_d",uptime_s,observed,errors,http_requests,metric_count}` |
| `shutdown` | — | 置位退出标志，返回 `{status:"shutting_down"}` |

## Prometheus HTTP 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/metrics` | GET | Prometheus 文本格式（`text/plain; version=0.0.4`），含 `# HELP`/`# TYPE` 与毫秒时间戳 |
| `/health`、`/healthz` | GET | JSON 健康状态 `{status:"ok",uptime_sec,metrics_count}` |
| 其他 | — | 404 |

- 端口固定 9091（`OBSERVE_D_METRICS_PORT`），仅绑定非 Windows 平台（Windows 下
  HTTP 服务器不可用，指标仅能经 JSON-RPC 查询）；
- 每个抓取请求都会累加 `airy_observe_requests_total` 与
  `airy_observe_http_requests_total`。

## 配置

- 常量（编译期，`main.c`）：`OBSERVE_D_DEFAULT_PORT=8091`（仅 Windows TCP）、
  `OBSERVE_D_METRICS_PORT=9091`、`OBSERVE_D_MAX_METRICS=256`、
  `OBSERVE_D_HTTP_BACKLOG=16`、`OBSERVE_D_MAX_CONN=128`、
  `OBSERVE_D_MAX_BUFFER=65536`；
- 监听：Linux 固定 Unix socket `airy_runtime_dir_socket("observe.sock")`；
- 无配置文件、无环境变量开关。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`commons` 基础库、cJSON、线程；Windows
  额外 `ws2_32`；
- 构建产物：可执行文件 `observe_d`（单文件实现）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target observe_d
```

## 测试

CTest 测试（`observe_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `observe_d_test_observe_service` | 指标记录 / 查询 / 服务逻辑 |

```bash
ctest --test-dir build -R "observe_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
