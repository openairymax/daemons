# llm_d — LLM 服务守护进程（llm.* 命名空间）

> **模块路径**: `agentrt/daemons/llm_d/`
> **命名空间**: `llm.*`（gateway 转发时剥离前缀，方法名不带 `llm.`）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/llm.sock`（TCP 8080 可选，Windows 命名管道 `\\.\pipe\airy_llm`）

## 定位

`llm_d` 是 AgentRT 的大模型服务守护进程，向上层（gateway、agent_d、CLI）提供统一的
模型调用、流式输出、Token 计数、Embeddings 代理与成本统计接口，屏蔽不同 LLM 提供商的
API 差异。内置多提供商适配（OpenAI / Anthropic / DeepSeek / Google / 本地模型）、
响应缓存、成本追踪与模型路由（cost_aware / round_robin / least_latency /
quality_first）。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket / TCP)
        ↓
  main.c（daemon_main 事件驱动样板 + 方法分发）
        ↓
  llm_service（缓存 / 成本追踪 / 路由 / 请求解析）
        ↓
  providers/（openai / anthropic / deepseek / google / local + registry）
```

- 事件驱动模型：`daemon_event_driver` 承载连接，线程池默认 8 线程、队列 256；
- 启动时若无 `--manager` 参数，自动回退加载 `${AIRY_CONFIG_DIR}/model.yaml`
  （与 think_d / gateway_d 一致）；
- 流式输出：`complete_stream` 通过 RS 帧（`0x1E`）分片推送增量，结束帧 `U`
  携带真实 usage（prompt/completion/reasoning tokens 与 `cost_usd`）；
- 时间感知注入：`complete` / `complete_stream` 在消息头注入宿主机当前时间
  （`YYYY-MM-DD 星期 HH:MM (UTC±N)` 格式的 system 消息），避免多轮拼接时间漂移；
  若首条 system 消息已含真实日期戳（`YYYY-MM-DD`）则跳过；
- 重试：`complete` 失败最多重试 3 次，指数退避（基础 100ms）。

## JSON-RPC 接口

以下方法表以 `main.c` 中 `method_dispatcher_register` 的真实注册为准（共 8 个方法）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `complete` | `model`(可选，默认取 `global.default_model`)、`messages`(必填，≤128 条，role/content/reasoning_content/tool_call_id/tool_calls)、`temperature`/`top_p`/`max_tokens`/`presence_penalty`/`frequency_penalty`/`stream`(可选)、`tools`(可选) | 非流式文本生成；失败指数退避重试 3 次 |
| `complete_stream` | 同 `complete` | 流式生成，RS 帧分片推送，结束发 usage 控制帧 |
| `list_models` | — | 返回模型清单 `{"models":[{name,provider,default}],"default_model",...}` |
| `count_tokens` | `text`(必填)、`model`(可选) | 按模型编码计数（claude→`claude`、gpt-3.5/text-davinci→`p50k_base`、其余默认 `cl100k_base`），返回 `{model,text,tokens,encoding}` |
| `embeddings` | `model`(可选)、`input` 等（OpenAI 格式原样转发） | 代理到所属 provider 的 `$api_base/embeddings`，返回上游 JSON |
| `health_check` | — | `{service:"llm_d",healthy,timestamp}` |
| `get_stats` | — | 服务统计（经 `llm_service_stats` 生成） |
| `shutdown` | — | 优雅关闭 |

## 配置

- **daemon 配置**（JSON，经 `--config` 传入，无则用默认值）：
  `daemon.socket_path`、`daemon.tcp_port`（设置即启用 TCP）、`daemon.max_threads`；
- **模型/提供商配置**：`model.yaml`（`$AIRY_CONFIG_DIR/model.yaml`），含
  provider/model 注册、`global.default_model`、`pricing` 定价规则
  （`pattern` / `input_price_per_k` / `output_price_per_k`）；
- 环境变量：`AIRY_LLM_D_DEBUG=1` 输出 DEBUG 日志；`AIRY_LLM_D_DIAG` 打印
  complete 发送诊断日志。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`commons` 基础库、libcurl（LLM 远端
  API）、cJSON、libyaml（可选，`HAVE_YAML` 开关）；Windows 额外 `ws2_32 bcrypt`；
- 构建产物：静态库 `airy_llm_service`（service/providers/router/…，供 llm_d 与
  测试共用）+ 可执行文件 `llm_d`。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target llm_d
```

## 测试

CTest 测试（`llm_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `llm_d_test_service` | 服务核心 |
| `llm_d_test_cache` | 响应缓存 |
| `llm_d_test_token_counter` | Token 计数 |
| `llm_d_test_response` | 响应构建/解析 |
| `llm_d_test_cost_tracker` | 成本追踪 |
| `llm_d_test_complexity_routing` | 复杂度路由 |
| `llm_d_test_routing_e2e` | 路由端到端 |
| `llm_d_test_router_integration` | 路由器集成（P3.16） |
| `llm_d_test_provider_reasoning` | reasoning_content 透传（DeepSeek/Kimi） |
| `llm_d_bench_routing_latency` | 路由延迟基准 |

```bash
ctest --test-dir build -R "llm_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
