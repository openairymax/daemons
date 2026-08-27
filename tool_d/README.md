# Tool Daemon — 工具执行守护进程

> **模块路径**: `agentrt/daemons/tool_d/`

## 定位

`tool_d` 是 AgentRT 的工具执行守护进程：负责工具注册/发现/查询/执行，内置一组
`builtin:*` 工具（fs / shell / web / git / maths），并通过参数校验、ACL 权限检查、
交互式审批、执行沙箱与结果缓存保障安全。它对外暴露 `tool.*` JSON-RPC 方法族
（L2 服务协议），是 Agent 与外部世界交互的执行层，gateway 的 MCP 协议工具
（`fs_read` 等）最终也转发到这里执行。

## 架构

```
gateway_d / 其他调用方 ──(tool.* JSON-RPC)──▶ tool_d
                                                │ tool_service（注册表 + 执行器 + 审批）
                                                │   ├─ registry：工具元数据 + 参数 Schema 校验
                                                │   ├─ executor：内置/外部执行 + 读写并发门
                                                │   │     ├─ READ 工具（读门）并发执行
                                                │   │     └─ WRITE 工具（写门）互斥串行
                                                │   ├─ tool_approval：交互式审批（pending/approve）
                                                │   ├─ os_sandbox：沙箱执行（Linux）
                                                │   └─ cache：结果缓存
                                                ▼
                                        builtin.c / builtin_{fs,shell,net,git,maths}.c
                                        maths_* → maths_d（maths.sock）
```

- 内置工具 15 个：`fs_read` `fs_write` `fs_list` `shell_run` `web_fetch` `fs_glob`
  `fs_grep` `fs_edit` `fs_delete` `web_search` `git_exec` `git_diff` `git_apply`
  `maths_eval`（转发 maths_d）`maths_stats`（转发 maths_d）；`executable` 使用
  `builtin:<id>` 标记，由执行器派发到真实实现。
- 参数校验（fail-closed）：注册表校验每个已注册参数的存在性与类型（JSON Schema），
  缺失/非法即拒绝执行。
- 读写并发门（P1d）：READ 类工具持读门并发运行；WRITE 类工具持写门互斥串行。
- 事件驱动开启 `concurrent_clients=true`：客户端请求派发到线程池并发处理，避免
  `execute_tool` 阻塞在 `approve` 决策上时 `pending`/`approve` 请求无法被处理。

## JSON-RPC 接口表

监听端点：Unix socket `$AIRY_RUNTIME_DIR/tool.sock`（`--tcp` 或 Windows 下为
TCP `127.0.0.1:8081`，Windows pipe `\\.\pipe\airy_tool`）。以下方法由
`method_dispatcher_register` 实际注册（main.c），共 11 个：

| 方法 | 参数 | 返回要点 | 说明 |
|------|------|----------|------|
| `register` | `tool`（对象：`id` `name` `executable` 必填，`description` `timeout_sec` `cacheable` `permission_rule` `params` 数组） | 成功即空 result | 注册外部工具；缺 id/name/executable 返回 -32602 |
| `list_tools` | 无 | 工具列表 JSON | 列出全部已注册工具 |
| `get_tool` | `tool_id` | 工具元数据（id/name/executable/description/timeout_sec/cacheable/permission_rule/params） | 查询单个工具；未找到 -32601 |
| `execute_tool` | `tool_id`、`params`（对象，必填）、`agent_id`（可选，用于 ACL 按真实主体判定） | `{"success":bool,"output"?, "error"?, "exit_code"}` | 执行工具；执行失败透传执行器错误描述（如「User denied tool execution」） |
| `execute` | 同 `execute_tool` | 同 `execute_tool` | `execute_tool` 的标准名别名 |
| `list` | 无 | 同 `list_tools` | `list_tools` 的标准名别名 |
| `pending` | 无 | `{"pending":[...]}` | 交互式审批挂起请求列表 |
| `approve` | `request_id`（必填）、`decision`（必填：`allow` / `always` / `deny`） | `{"resolved":true,"request_id","decision"}` | 审批决策；非法 decision -32602，请求不存在 -32602 |
| `health_check` | 无 | `{"service":"tool_d","healthy","timestamp"}` | L2 标准方法 |
| `shutdown` | 无 | — | L2 标准方法，触发优雅退出 |
| `get_stats` | 无 | 统计 JSON（`tool_service_get_stats`） | daemon 级统计 |

审批语义（executor）：无 ACL 授权或需人工确认时挂起并产生 `request_id` →
`pending` 可查询 → `approve` 决策：`allow` 单次放行；`always` 放行并写入持久 ACL
规则；`deny` 拒绝（执行失败描述「User denied tool execution」）。

## 配置

- 命令行：`--manager <config>` / `-c <config>`（配置文件）；`--tcp` 强制 TCP。
- daemon 配置为 JSON 文件：`daemon.socket_path` / `daemon.tcp_port`（设置后启用 TCP）/
  `daemon.max_clients`（默认 64）。
- 服务配置默认路径：`agentrt/manager/service/tool_d/tool.yaml`（`AIRY_HAS_YAML` 时解析）。
- ACL 双来源（fail-closed：无 ACL 条目即拒绝）：
  1. 权威源 `$AIRY_CONFIG_DIR/permission_rules.yaml`（`daemon_security_init` 加载，
     默认随 `AIRY_HOME` 初始化落地；文件缺失仅告警不阻断启动）；
  2. 环境变量 `AIRY_AGENT_ACL` 静态预授权，格式
     `agent=tool1,tool2,...;agent2=tool3`（daemon 由 `/daemon start` 拉起时环境变量
     易丢失，故以文件为权威源）。
  内置工具默认对身份 `tool_d` 注册 ACL 规则；执行时若调用方传入 `agent_id`，ACL 按真实
  主体判定。
- 事件驱动：线程池 4~8，队列 256，`max_events=64`，`concurrent_clients=true`。

## 依赖与构建

- 依赖：`airy_tool_service`（service/registry/executor/validator/cache/tool_approval/
  safety_guard_bridge/tool_svc_adapter/builtin{,_fs,_shell,_net,_git,_maths}/
  tool_interactive_approval/os_sandbox 静态库）、`svc_common`、`cupolas`（SafetyGuard）、
  `airy_syscall`（sandbox API）、`airy_coreloopthree`（GNU ld 下 `--whole-archive`）、
  `airy_cognition` / `airy_core` / `airy_memory` / `airy_atoms` / `airy_common` /
  `airy_llm_service`、`Threads::Threads`、`airy_platform_libs`；可选 `cJSON` / `YAML`。
- 沙箱：`os_sandbox` 基于 Landlock/seccomp/unshare/mount（Linux-only）；macOS/BSD 为
  空实现（`OS_SANDBOX_MODE_OFF`）。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target tool_d
```

## 测试

- `tests/`（`tool_d_*` ctest 用例）：`test_service` / `test_registry` /
  `test_executor` / `test_validator` / `test_cache` / `test_sandbox_integration` /
  `test_fs_e2e`；`test_os_sandbox` 仅 Linux（依赖 Landlock/seccomp）。
- 运行：

```bash
ctest --test-dir build -R "tool_d_" -V
```
