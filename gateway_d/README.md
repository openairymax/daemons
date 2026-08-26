# Gateway Daemon — API 网关守护进程

> **模块路径**: `agentrt/daemons/gateway_d/`

## 定位

`gateway_d` 是 AgentRT 的 API 网关守护进程，也是外部世界与内部 daemon 集群之间的唯一
流量入口。它对外提供 HTTP / WebSocket / Stdio 三种传输，对内做协议转换与命名空间转发：
识别 MCP、A2A、OpenAI 兼容协议并翻译为内部 L2 服务协议（`<daemon>.<method>`）调用，
或按 `llm.*` / `mem.*` / `agent.*` / `tool.*` / `sched.*` 等命名空间白名单转发到对应
daemon。协议翻译集中在网关层，daemon 无需感知外部协议（D2）。

## 架构

```
客户端 ──HTTP(8080)/WS(8081)/Stdio──▶ gateway_d
                                        │ gateway_service（HTTP/WS/Stdio 网关实例）
                                        ▼
                                  gateway_protocol_entry（协议检测）
                     ┌───────────────┬──────────────┬──────────────┐
                     ▼               ▼              ▼              ▼
                MCP 处理器       A2A 处理器    OpenAI 兼容      JSON-RPC 业务分发
             (9 内置工具 +     (tasks/send→   (chat/completions  (gateway_business_handle)
              外部 MCP 客户端)   sched_d)      →llm_d.complete,
                                              embeddings→llm_d)
                     │                            │
                     ▼                            ▼
             tool_d（fs_*/shell_run/...）   llm_d / sched_d
             命名空间转发（16 个）：llm.* agent.* mem.* tool.* a2a.* plugin.* info.*
             notify.* observe.* market.* hook.* sched.* think.* monit.* channel.*
             cupolas.*  → 各自 daemon socket；hall.* 在网关内实现
```

- 所有 gateway→daemon 派发经微核心统一派发钩子 `airy_sys_svc_call()`（SYS_SVC_CALL，
  架构约束 2026-08-25「必须走 syscall」）。
- daemon 端点解析：`<DAEMON>_SOCK` 环境变量覆盖 → `$AIRY_RUNTIME_DIR/<name>.sock` →
  `$AIRY_HOME/run/<name>.sock`；Windows 走 TCP 回环（`127.0.0.1:<port>`）。
- 主循环 1s 轮询；`SIGINT/SIGTERM` 或 `shutdown` RPC 触发优雅退出；`SIGUSR1` 切换日志级别。

## JSON-RPC 接口表

gateway 不采用 `method_dispatcher_register`，而是统一经 `gateway_protocol_entry`
（协议检测）→ `gateway_business_handle`（JSON-RPC 业务分发）。直接在网关内处理的方法：

| 方法 | 处理方式 |
|------|----------|
| `agent.run` / `agent.cancel` | 网关内编排（双思考注入 + 取消，见 `gateway_biz_agent.c`） |
| `llm.list_models` | 网关内转发 llm_d `list_models` 并附加 `default_model/default_provider` |
| `tool.pending` / `tool.approve` | 网关内转发 tool_d 审批流程（approve 校验 `request_id`+`decision`） |
| `ping` | 返回 `{"status":"ok"}` |
| `shutdown` | 返回 `{"status":"shutting_down"}` 后触发优雅退出 |
| `hall.*` | 网关内实现（任务看板/事件流/决策链，读 `$AIRY_HOME` 持久化 work-hall board + hall-store 事件文件 + 实时 `agent.list`） |

### 命名空间转发（白名单内方法透传，其余返回 -32601）

| 命名空间 | 白名单方法 | 目标 |
|----------|-----------|------|
| `llm.*` | `complete` `list_models` `count_tokens` `health_check` `get_stats` | llm_d |
| `agent.*` | `spawn` `terminate` `invoke` `cancel` `list` `count` `health_check` `get_stats` | agent_d |
| `mem.*` | `write` `search` `get` `delete` `count` `recent` `evolve` `health_check` `get_stats` `kb_ingest` `kb_search` `kb_delete` `kb_list` | mem_d（`AIRY_GATEWAY_MEM_PUBLIC=false` 时拒绝，-32001） |
| `tool.*` | `register` `list_tools` `get_tool` `execute_tool` `execute` `list` `health_check` `get_stats` | tool_d |
| `a2a.*` | `register_agent` `unregister_agent` `discover_agents` `create_task` `update_task` `cancel_task` `get_task` `send_message` `count` `send` `receive` `health_check` `get_stats` | a2a_d |
| `plugin.*` | `load` `unload` `start` `stop` `execute` `get_metadata` `get_state` `get_stats` `list` `install` `uninstall` `health_check` | plugin_d |
| `info.*` | `system` `history` `health` `health_check` `get_stats` | info_d |
| `notify.*` | `publish` `subscribe` `unsubscribe` `list` `health` `health_check` `get_stats` | notify_d |
| `observe.*` | `record_metric` `query_metrics` `get_metrics` `get_stats` `health_check` | observe_d |
| `market.*` | `register_agent` `search_agents` `install_agent` `register_skill` `search_skills` `health_check` `publish` `search` `install` `get_stats` | market_d |
| `hook.*` | `register` `unregister` `trigger` `list` `status` `stats` `health` `ping` `health_check` `get_stats` | hook_d |
| `sched.*` | `register_agent` `unregister_agent` `schedule_task` `get_task` `cancel` `dag_submit` `dag_status` `dag_cancel` `checkpoint_save` `submit` `query` `get_stats` `health_check` | sched_d |
| `think.*` | `process` `orchestrate` `health_check` `get_stats` | think_d |
| `monit.*` | `record_metric` `get_metrics` `trigger_alert` `get_alerts` `health_check` `generate_report` `heartbeat` `metrics` `alert_raise` `alert_resolve` `get_stats` | monit_d |
| `channel.*` | `ping` `list` `open` `close` `send` `health` `health_check` `get_stats` | channel_d |
| `cupolas.*` | `check_permission` `sanitize` `execute_command` `add_rule` `audit_flush` `health_check` `get_stats` `vault_store` `vault_retrieve` `vault_delete` `vault_list` `vault_rotate` `net_add_rule` `net_check_access` `net_get_stats` `entitlements_load` `entitlements_check` | cupolas_d |

转发语义：params/响应原样透传，响应 `id` 重写为请求 `id`（JSON-RPC 2.0 并发合规）。

### 外部协议（AIRY_ENABLE_PROTOCOLS 开启，协议栈来自 `protocols/`）

| 协议 | 识别 | 处理 |
|------|------|------|
| MCP | 路径 `/mcp`，或 JSON-RPC 方法 `initialize` / `tools/list` / `tools/call` / `resources/list` / `resources/read` / `prompts/list` / `notifications/initialized` | 内置 9 工具（`fs_read` `fs_write` `fs_list` `shell_run` `web_fetch` `fs_glob` `fs_grep` `fs_edit` `web_search`）→ tool_d；外部 MCP 服务器经 `AIRY_MCP_CLIENTS` 环境变量（JSON 数组，stdio/http transport）注册为 `<client>_<tool>` 前缀工具并转发 |
| A2A | 路径 `/a2a`，或 JSON-RPC 方法 `tasks/send` `tasks/get` `tasks/cancel` `tasks/pushNotification` `message/send` `agent-card/get` `agent/getAgentCard` | 任务类型 `coding` `analysis` `summarize` `general` `devops` → sched_d |
| OpenAI | 路径 `/v1/` `/openai`，或 body 含 `model`+`messages`；`text/event-stream` | `chat/completions` → llm_d `complete`；`embeddings` → llm_d `embeddings` |

协议检测优先级：路径 → Content-Type → body。非以上协议的 JSON-RPC 请求落入业务分发。

## 配置

- 默认配置：HTTP `0.0.0.0:8080`（enabled，`max_request_size=1MB`，`timeout_ms=30000`）、
  WebSocket `0.0.0.0:8081`（enabled）、Stdio 默认关闭。
- 配置文件（`-c <path>`）为 **key=value 行格式**（`gateway_service_load_config`，
  不解析 YAML）：`http.port` `http.host` `http.enabled` `stdio.max_request_size`
  `stdio.timeout_ms` 等。
- 命令行参数：

| 参数 | 说明 |
|------|------|
| `-c <config>` | key=value 配置文件 |
| `-h <host>` | HTTP host（默认 0.0.0.0） |
| `-p <port>` | HTTP 端口（默认 8080） |
| `-w <port>` | WebSocket 端口（默认 8081） |
| `-s` | 启用 Stdio 网关 |
| `-d` | 守护化（Unix） |
| `-v` | 启用指标（周期健康检查日志，30s） |
| `--manager <config>` | 兼容 bootstrap 统一参数（忽略） |
| `--help` | 帮助 |

- 环境变量：

| 环境变量 | 作用 |
|----------|------|
| `AIRY_*_SOCK`（llm/tool/agent/mem/sched/think/a2a/plugin/info/notify/observe/market/hook/monit/channel/cupolas） | 覆盖对应 daemon 端点 |
| `AIRY_LLM_TCP_ADDR` / `AIRY_LLM_TCP_PORT` | LLM TCP 地址/端口 |
| `AIRY_AGENT_MODEL` | 覆盖默认模型（其次 `$AIRY_CONFIG_DIR/model.yaml` 的 `global.default_model`） |
| `AIRY_MCP_CLIENTS` | 外部 MCP 服务器 JSON 数组（stdio/http） |
| `AIRY_GATEWAY_DISABLE_WS` | 非空即禁用 WebSocket 网关 |
| `AIRY_GATEWAY_ACL_ALLOW_SHELL` | `false`/`0` 时 ACL 拒绝 `shell_run`（默认允许） |
| `AIRY_GATEWAY_MEM_PUBLIC` | `false`/`0` 时拒绝外部 `mem.*` 访问（-32001） |

- ACL（fail-closed）：外部协议请求统一使用身份 `external`，默认注册
  `fs_read` `fs_write` `fs_list` `web_fetch` `fs_glob` `fs_grep` `fs_edit` `web_search`
  为允许，`shell_run` 默认允许（可经 `AIRY_GATEWAY_ACL_ALLOW_SHELL` 拒绝）。

## 依赖与构建

- 依赖：`airy_gateway_service`（service / gateway_svc_adapter / gateway_business_handler /
  gateway_biz_{forward,llm,agent,backend,hall,svcdispatch} + protocol/{gateway_mcp_server,
  gateway_a2a_handler,gateway_openai_compat,gateway_protocol_router}）、`gateway_lib_obj`
  （`gateway/` 模块：http_gateway / ws_gateway / stdio_gateway / http2_gateway* /
  jsonrpc / syscall_router_*）、`airy_protocols`、`airy_syscall`、`svc_common`；
  可选 `microhttpd`（HTTP）、`libwebsockets`（WS）、`nghttp2`（HTTP/2）。
- 协议栈开关：`AIRY_ENABLE_PROTOCOLS`（默认 ON，定义 `AIRY_HAS_PROTOCOLS`）。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target gateway_d
```

## 测试

- `tests/test_protocol_router.c`：协议路由/检测逻辑。
- `tests/test_gateway_hall_store.c`：hall 存储层。
- `tests/test_service.c`：gateway 服务生命周期。
- 运行：

```bash
ctest --test-dir build -R "gateway_d" -V
```
