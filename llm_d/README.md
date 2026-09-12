# llm_d — 大模型服务守护进程

> **模块路径**：`agentrt/daemons/llm_d/` · **可执行文件 / CMake 目标**：`llm_d` · **RPC 命名空间**：`llm.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`llm_d` 是 AgentRT 的大模型服务进程，对上层（`gateway_d`、`agent_d`、`think_d`、CLI）
提供统一的模型调用、流式输出、Token 计数、Embeddings 代理与成本统计接口，屏蔽不同
提供商的 API 差异。

- 端点：POSIX Unix socket `<runtime-dir>/llm.sock`（`$AIRY_HOME/run/llm.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8080`。
- 可选 TCP：POSIX 上默认端口 `8080`（配置 `daemon.tcp_port` 或 `--tcp` 时启用）。

## 能力

- **多提供商适配** — OpenAI / Anthropic / DeepSeek / Google / 本地模型，统一经 provider
  registry 调度，`reasoning_content` 等扩展字段透传。
- **模型路由** — `cost_aware` / `round_robin` / `least_latency` / `quality_first` 四种策略。
- **流式输出** — `complete_stream` 以 RS 帧（`0x1E`）分片推送增量，结束帧携带真实
  usage（prompt / completion / reasoning tokens 与 `cost_usd`）。
- **响应缓存与成本追踪** — 命中缓存不重复计费；按 `pricing` 规则计算成本。
- **Token 计数** — 按模型选择编码（claude 系列、`p50k_base`、默认 `cl100k_base`）。
- **重试与时间感知注入** — `complete` 失败最多重试 3 次指数退避（基础 100 ms）；
  在缺少真实日期戳时注入宿主机当前时间，避免多轮拼接的时间漂移。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket / TCP)
        ↓
  main.c（事件驱动样板 + 方法分发）
        ↓
  airy_llm_service（service / cache / cost_tracker / token_counter / response）
        ├── providers/  openai · anthropic · deepseek · google · local + registry
        └── router/     cost_aware · round_robin · least_latency · quality_first
```

- 服务层抽为静态库 `airy_llm_service`，供 `llm_d` 可执行文件与测试共用；
- 事件驱动：`daemon_event_driver` 承载连接，线程池默认 8 线程、队列 256；
- 启动时若未指定 `--manager`，自动回退加载 `$AIRY_CONFIG_DIR/model.yaml`
  （与 `think_d`、`gateway_d` 一致）。

## JSON-RPC 接口

共 8 个方法，经 `method_dispatcher_register` 注册（方法名不含命名空间前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `complete` | `{model?, messages: [≤128 条 role/content/reasoning_content/tool_call_id/tool_calls], temperature?, top_p?, max_tokens?, presence_penalty?, frequency_penalty?, stream?, tools?}` | 补全文本 + usage | 非流式生成，`model` 缺省取 `global.default_model`；失败指数退避重试 3 次 |
| `complete_stream` | 同 `complete` | RS 帧分片 | 流式生成，结束帧携带 usage 与成本 |
| `list_models` | `{}` | `{models: [{name, provider, default}], default_model, ...}` | 模型清单 |
| `count_tokens` | `{text, model?}` | `{model, text, tokens, encoding}` | 按模型编码计数 |
| `embeddings` | `{model?, input, ...}`（OpenAI 请求格式原样转发） | 上游 JSON | 代理到所属 provider 的 `/embeddings` |
| `health_check` | `{}` | `{service, healthy, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | 统计 JSON | 请求数、缓存命中、成本等 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`llm.complete`），由 `gateway_d` 剥离前缀后转发。

## 配置

daemon 配置（JSON，经 `--manager <config>` 传入，未指定时使用内建默认值）：
`daemon.socket_path`、`daemon.tcp_port`、`daemon.max_threads`。

模型与提供商配置在 `model.yaml`（`$AIRY_CONFIG_DIR/model.yaml`），包含 provider /
model 注册、`global.default_model`，以及 `pricing` 定价规则
（`pattern` / `input_price_per_k` / `output_price_per_k`）。

| 环境变量 | 说明 |
|----------|------|
| `AIRY_LLM_D_DEBUG=1` | 输出 DEBUG 级日志 |
| `AIRY_LLM_D_DIAG` | 打印 `complete` 请求发送诊断 |
| `AIRY_LLM_SOCK` | 网关侧覆盖 llm_d 端点 |

## 用法

```bash
airymaxrt logs llm_d          # 运行态日志
<build-dir>/bin/llm_d         # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target llm_d
ctest --test-dir ../daemons-build -R "llm_d_" --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

CTest 用例（`llm_d_*`）：

| 用例 | 覆盖点 |
|------|--------|
| `llm_d_test_service` | 服务核心 |
| `llm_d_test_cache` | 响应缓存 |
| `llm_d_test_token_counter` | Token 计数 |
| `llm_d_test_response` | 响应构建与解析 |
| `llm_d_test_cost_tracker` | 成本追踪 |
| `llm_d_test_complexity_routing` | 复杂度路由 |
| `llm_d_test_routing_e2e` | 路由端到端 |
| `llm_d_test_router_integration` | 路由器集成 |
| `llm_d_test_provider_reasoning` | `reasoning_content` 透传 |
| `llm_d_bench_routing_latency` | 路由延迟基准 |

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher、`svc_model_defaults` |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、HTTP、cJSON 封装 |
| [corekern](https://atomgit.com/openairymax/atoms) | `airy_init()` 核心引导 |
| libcurl | 调用 LLM 远端 API |
| libyaml | 解析 `model.yaml`（`HAVE_YAML` 时启用） |
| [gateway_d](../gateway_d/README.md) | 上游：`llm.*` 命名空间转发方 |

## 关系

`llm_d` 只做模型协议与用量层，不管理 Agent 生命周期，也不做推理规划：
认知编排由 `think_d` 负责，Agent 运行由 `agent_d` 负责，二者经 `gateway_d` 调用本服务。
`model.yaml` 与 `gateway_d` 共用同一份默认模型解析实现（`svc_model_defaults`）。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
