# gateway_d — API 网关守护进程

> **模块路径**：`agentrt/daemons/gateway_d/` · **可执行文件 / CMake 目标**：`gateway_d`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`gateway_d` 是 AgentRT 的 API 网关守护进程，也是外部世界与内部 daemon 集群之间的
**唯一流量入口**。它对外提供 HTTP / WebSocket / Stdio 三种传输，对内只做协议转换与
按命名空间的白名单转发：识别 MCP、A2A、OpenAI 兼容协议并翻译为内部服务协议
（`<daemon>.<method>`）调用，或把 `llm.*` / `mem.*` / `agent.*` / `tool.*` / `sched.*`
等命名空间转发到对应 daemon。

协议翻译集中在网关层，后端 daemon 不感知任何外部协议；网关本身不含业务逻辑。

## 能力

- **多传输接入** — HTTP（默认 `8080`）、WebSocket（默认 `8081`）、Stdio。
- **外部协议适配** — MCP、A2A、OpenAI Chat Completions / Embeddings 三种协议自动识别，
  检测优先级为路径 → Content-Type → body。
- **命名空间路由** — 18 个外部命名空间白名单转发，白名单外方法返回 `-32601`。
- **并发合规透传** — params 与响应原样透传，响应 `id` 重写为请求 `id`。
- **fail-closed ACL** — 外部协议请求统一使用身份 `external`，按工具粒度授权。
- **凭据与模型解析** — 默认模型/Provider 解析链、LLM 后端端点解析链。

## 架构

```
客户端 ──HTTP(8080)/WS(8081)/Stdio──▶ gateway_d
                                        │ gateway_service（HTTP/WS/Stdio 网关实例）
                                        ▼
                                  gateway_protocol_entry（协议检测）
                     ┌───────────────┬──────────────┬──────────────┐
                     ▼               ▼              ▼              ▼
                MCP 处理器       A2A 处理器    OpenAI 兼容      JSON-RPC 业务分发
             (9 内置工具 +     (tasks/send →  (chat/completions  (gateway_business_handle)
              外部 MCP 客户端)   sched_d)       → llm_d.complete,
                                              embeddings → llm_d)
                     │                            │
                     ▼                            ▼
             tool_d（fs_* / shell_run / …）  llm_d / sched_d

             命名空间转发（18 个）：llm.* agent.* mem.* tool.* a2a.* plugin.* info.*
             notify.* observe.* market.* hook.* sched.* think.* monit.* channel.*
             cupolas.* policy.* maths.*  → 各自 daemon 端点；hall.* 在网关内实现
```

- 所有 gateway → daemon 派发统一经微核心系统调用 `airy_sys_svc_call()`（`SYS_SVC_CALL`）
  下派，网关层不直接持有后端连接。
- 后端端点解析顺序：`<DAEMON>_SOCK` 环境变量覆盖 → `$AIRY_RUNTIME_DIR/<name>.sock` →
  `$AIRY_HOME/run/<name>.sock`；Windows 走 TCP 回环（`127.0.0.1:<port>`）。
- 主循环 1s 轮询；`SIGINT` / `SIGTERM` 或 `shutdown` RPC 触发优雅退出；
  `SIGUSR1` 切换日志级别。

## JSON-RPC 接口

`gateway_d` 不使用 `method_dispatcher_register`，而是统一经 `gateway_protocol_entry`
（协议检测）→ `gateway_business_handle`（JSON-RPC 业务分发）。在网关内直接处理的方法：

| 方法 | 处理方式 |
|------|----------|
| `agent.run` / `agent.cancel` | 网关内编排（双思考注入 + 取消） |
| `llm.list_models` | 转发 llm_d `list_models` 并附加 `default_model` / `default_provider` |
| `tool.pending` / `tool.approve` | 转发 tool_d 审批流程（`approve` 校验 `request_id` + `decision`） |
| `ping` | 返回 `{"status":"ok"}` |
| `shutdown` | 返回 `{"status":"shutting_down"}` 后触发优雅退出 |
| `hall.*` | 网关内实现（任务看板 / 事件流 / 决策链：读 `$AIRY_HOME` 下的 work-hall board、hall-store 事件文件，并实时合并 `agent.list`） |

### 命名空间转发白名单

白名单由网关的能力登记表（`gateway_cap_registry.c`）集中声明，启动期做 namespace
独占归属校验；每个命名空间唯一归属一个 daemon，白名单外方法返回 `-32601`。

| 命名空间 | 白名单方法（wire 方法名） | 目标 |
|----------|-----------|------|
| `llm.*` | `complete` `list_models` `count_tokens` `health_check` `get_stats` | llm_d |
| `agent.*` | `run` `cancel`（网关内编排）`spawn` `terminate` `invoke` `list` `count` `health_check` `get_stats` | agent_d |
| `mem.*` | `write` `search` `get` `delete` `count` `recent` `evolve` `health_check` `get_stats` `kb_ingest` `kb_search` `kb_delete` `kb_list` | mem_d（`AIRY_GATEWAY_MEM_PUBLIC=false` 时拒绝，`-32001`） |
| `tool.*` | `pending` `approve`（网关内审批）`register` `list_tools` `get_tool` `execute_tool` `execute` `list` `health_check` `get_stats` | tool_d |
| `a2a.*` | `register_agent` `unregister_agent` `discover_agents` `create_task` `update_task` `cancel_task` `get_task` `send_message` `count` `send` `receive` `health_check` `get_stats` | a2a_d |
| `plugin.*` | `load` `unload` `start` `stop` `execute` `get_metadata` `get_state` `get_stats` `list` `install` `uninstall` `health_check`（wire 加 `plugin_` 前缀） | tool_d（插件域由 tool_d 独占） |
| `info.*` | `system` `history` `health` `hardware`；`health_check` / `get_stats` 透传宿主 | monit_d（wire 加 `info_` 前缀） |
| `notify.*` | `publish` `subscribe` `unsubscribe` `list` `health` `health_check` `get_stats` | notify_d |
| `observe.*` | `record_metric` `query_metrics` `get_metrics`；`get_stats` / `health_check` 透传宿主 | monit_d（wire 加 `observe_` 前缀） |
| `market.*` | `register_agent` `search_agents` `install_agent` `register_skill` `search_skills` `health_check` `publish` `search` `install` `get_stats` | market_d |
| `hook.*` | `register` `unregister` `trigger` `list` `status` `stats` `health` `ping` `health_check` `get_stats` | hook_d |
| `sched.*` | `register_agent` `unregister_agent` `schedule_task` `get_task` `cancel` `dag_submit` `dag_status` `dag_list` `dag_cancel` `checkpoint_save` `submit` `query` `get_stats` `health_check` `plan` `absorb` `roadmap_stats` | sched_d |
| `think.*` | `process` `orchestrate` `health_check` `get_stats` `lang_process` `lang_postprocess` `lang_stats` `review` | think_d |
| `monit.*` | `record_metric` `get_metrics` `trigger_alert` `get_alerts` `health_check` `generate_report` `heartbeat` `metrics` `alert_raise` `alert_resolve` `get_stats` | monit_d |
| `channel.*` | `ping` `list` `open` `close` `send` `health` `health_check` `get_stats` | channel_d |
| `cupolas.*` | `check_permission` `sanitize` `execute_command` `add_rule` `audit_flush` `health_check` `get_stats` `vault_store` `vault_retrieve` `vault_delete` `vault_list` `vault_rotate` `net_add_rule` `net_check_access` `net_get_stats` `entitlements_load` `entitlements_check` | cupolas_d |
| `policy.*` | `load` `activate` `rollback` `status`（wire 为 `policy_*`） | cupolas_d（策略域的独占持有者） |
| `maths.*` | `health_check` | maths_d |
| `hall.*` | `board` `tasks` `replay` `stream` | 网关内实现，不转发 |

### 外部协议

外部协议由 `AIRY_ENABLE_PROTOCOLS` 开关控制（默认 ON，定义 `AIRY_HAS_PROTOCOLS`），
协议栈来自 [protocols](https://atomgit.com/openairymax/protocols)。

| 协议 | 识别 | 处理 |
|------|------|------|
| MCP | 路径 `/mcp`，或 JSON-RPC 方法 `initialize` / `tools/list` / `tools/call` / `resources/list` / `resources/read` / `prompts/list` / `notifications/initialized` | 内置 9 工具（`fs_read` `fs_write` `fs_list` `shell_run` `web_fetch` `fs_glob` `fs_grep` `fs_edit` `web_search`）→ tool_d；外部 MCP 服务器经 `AIRY_MCP_CLIENTS` 环境变量（JSON 数组，stdio / http transport）注册为 `<client>_<tool>` 前缀工具并转发 |
| A2A | 路径 `/a2a`，或 JSON-RPC 方法 `tasks/send` `tasks/get` `tasks/cancel` `tasks/pushNotification` `message/send` `agent-card/get` `agent/getAgentCard` | 任务类型 `coding` `analysis` `summarize` `general` `devops` → sched_d |
| OpenAI | 路径 `/v1/` `/openai`，或 body 含 `model` + `messages`；`text/event-stream` | `chat/completions` → llm_d `complete`；`embeddings` → llm_d `embeddings` |

非以上协议的 JSON-RPC 请求落入业务分发。

## 配置

- 默认监听：HTTP `0.0.0.0:8080`（enabled，`max_request_size=1MB`，`timeout_ms=30000`）、
  WebSocket `0.0.0.0:8081`（enabled）、Stdio 默认关闭。
- 配置文件（`-c <path>`）为 **key=value 行格式**（`gateway_service_load_config`，不解析
  YAML）：`http.port` `http.host` `http.enabled` `stdio.max_request_size`
  `stdio.timeout_ms` 等。
- 命令行参数：

| 参数 | 说明 |
|------|------|
| `-c <config>` | key=value 配置文件 |
| `-h <host>` | HTTP host（默认 `0.0.0.0`） |
| `-p <port>` | HTTP 端口（默认 `8080`） |
| `-w <port>` | WebSocket 端口（默认 `8081`） |
| `-s` | 启用 Stdio 网关 |
| `-d` | 守护化（Unix） |
| `-v` | 启用指标（30s 周期健康检查日志） |
| `--manager <config>` | 兼容统一启动参数（忽略） |
| `--help` | 帮助 |

- 环境变量：

| 环境变量 | 作用 |
|----------|------|
| `AIRY_<NS>_SOCK`（LLM / TOOL / AGENT / MEM / SCHED / THINK / A2A / NOTIFY / MARKET / HOOK / MONIT / CHANNEL / CUPOLAS / MATHS） | 覆盖对应 daemon 端点 |
| `AIRY_LLM_TCP_ADDR` / `AIRY_LLM_TCP_PORT` | LLM TCP 地址 / 端口 |
| `AIRY_AGENT_MODEL` | 覆盖默认模型（其次 `$AIRY_CONFIG_DIR/model.yaml` 的 `global.default_model`） |
| `AIRY_MCP_CLIENTS` | 外部 MCP 服务器 JSON 数组（stdio / http） |
| `AIRY_GATEWAY_DISABLE_WS` | 非空即禁用 WebSocket 网关 |
| `AIRY_GATEWAY_ACL_ALLOW_SHELL` | `false` / `0` 时 ACL 拒绝 `shell_run`（默认允许） |
| `AIRY_GATEWAY_MEM_PUBLIC` | `false` / `0` 时拒绝外部 `mem.*` 访问（`-32001`） |

- ACL（fail-closed）：外部协议请求统一使用身份 `external`，默认允许
  `fs_read` `fs_write` `fs_list` `web_fetch` `fs_glob` `fs_grep` `fs_edit` `web_search`；
  `shell_run` 默认允许，可经 `AIRY_GATEWAY_ACL_ALLOW_SHELL` 拒绝。

## 用法

运行时整体由 `airymaxrt` 启动器管理，通常不需要手工拉起网关。运行态排查：

```bash
airymaxrt status          # 运行时状态
airymaxrt doctor          # 组件健康检查
airymaxrt logs gateway_d  # 网关日志
```

手工启动单个进程并验证接口：

```bash
<build-dir>/bin/gateway_d -p 8080
curl -s http://127.0.0.1:8080/ -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
```

排查要点：某个命名空间返回 `-32601` 说明方法不在白名单内；返回连接类错误说明目标
daemon 未启动或端点解析失败，检查对应的 `AIRY_*_SOCK` 与运行目录。

## 依赖与构建

- 依赖：`airy_gateway_service`（service / gateway_svc_adapter / gateway_business_handler
  及 forward / llm / agent / backend / hall / svcdispatch 业务编排，protocol 下的
  MCP server / A2A handler / OpenAI 兼容 / 协议路由）、`gateway_lib_obj`
  （[gateway](https://atomgit.com/openairymax/gateway) 模块：http_gateway / ws_gateway /
  stdio_gateway / http2_gateway / jsonrpc / syscall_router）、
  [airy_protocols](https://atomgit.com/openairymax/protocols)、
  [airy_syscall](https://atomgit.com/openairymax/atoms)、`svc_common`；
  可选 `microhttpd`（HTTP）、`libwebsockets`（WS）、`nghttp2`（HTTP/2）。
- 构建（在源码树之外创建构建目录）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target gateway_d
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

- `tests/test_protocol_router.c`：协议路由与检测逻辑。
- `tests/test_gateway_hall_store.c`：hall 存储层。
- `tests/test_service.c`：gateway 服务生命周期。

```bash
ctest --test-dir ../daemons-build -R "gateway_d" -V
```

## 关系

| 方向 | 对象 | 说明 |
|------|------|------|
| 上游 | [gateway](https://atomgit.com/openairymax/gateway) | 被 `gateway_d` 包装为服务的网关库 |
| 上游 | [protocols](https://atomgit.com/openairymax/protocols) | MCP / A2A / OpenAI 协议适配 |
| 上游 | [atoms](https://atomgit.com/openairymax/atoms) | `airy_sys_svc_call()` 系统调用派发 |
| 上游 | [commons](https://atomgit.com/openairymax/commons) | 平台路径、配置、日志 |
| 下游 | 14 个后端 daemon | 按命名空间白名单转发 |
| 下游 | SDK / 命令行 / 终端 UI | 网关 JSON-RPC 表面的调用方 |

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
