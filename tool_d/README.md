# tool_d — 工具执行守护进程

> **模块路径**：`agentrt/daemons/tool_d/` · **可执行文件 / CMake 目标**：`tool_d` · **RPC 命名空间**：`tool.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`tool_d` 是 AgentRT 的**工具执行层**：承载工具注册表、参数校验、权限判定、
读写并发门、执行沙箱、结果缓存与交互式审批，并内置一组 `builtin:*` 工具
（文件系统 / shell / 网络 / git / 数学）。动态插件（`dlopen` 执行域）同样运行在
本进程内，`plugin_*` 方法登记于 `tool.*` 命名空间。它是 Agent 与外部世界交互的
唯一落地点，`gateway_d` 的 MCP 工具目录与执行请求最终都转发到这里。

- 端点：POSIX Unix socket `<runtime-dir>/tool.sock`（`$AIRY_HOME/run/tool.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8081`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8081`（默认只监听 socket）。
- 请求缓冲上限 64 KiB；事件循环 `max_events` 64，工作线程池 4~8、队列 256。
- 内置工具 15 个；结果缓存容量 1024 条、TTL 3600 秒。

## 能力

- **工具注册与发现** — 外部工具经 `register` 声明元数据与参数 JSON Schema，
  `list_tools` / `get_tool` 查询目录；内置工具在 `tool_service_create()` 时全部注册，
  `executable` 使用 `builtin:<id>` 标记，由执行器派发到真实实现。
- **参数校验（fail-closed）** — 每个已注册参数按其 JSON Schema 校验存在性与类型，
  缺失或非法即拒绝执行。
- **权限判定** — 执行前查 ACL：`(agent_id, tool)` 无授权条目即拒绝。ACL 有两个来源，
  文件为权威源、环境变量为预授权补充（见「配置」）。
- **读写并发门** — `TOOL_ACCESS_READ` 工具持读门并发执行，`TOOL_ACCESS_WRITE`
  工具持写门互斥串行，避免写写并发破坏外部状态。
- **交互式审批** — 置 `AIRY_TOOL_APPROVAL_MODE=interactive` 后，未授权工具的执行
  挂起并产生 `request_id`，由 `pending` 查询、`approve` 决策（默认关闭，服务端
  部署走 ACL 静态授权）。
- **OS 沙箱** — `shell_run` 等子进程执行经 Landlock / seccomp / unshare / mount
  围堵（Linux）；文件类工具的路径围堵以同一 workspace 为准，越界路径一律拒绝。
- **结果缓存** — `cacheable` 工具按参数指纹复用结果。
- **动态插件** — 启动时扫描 `$AIRY_HOME/ecosystem/plugins`，manifest 有效且权限
  校验通过的插件被加载并启动；插件生命周期与调用在本进程内承载。

## 架构

```
CLI / SDK ──▶ gateway_d ──(tool.* / plugin.* JSON-RPC)──▶ tool_d
                                                          │
              tool_service ───────────────────────────────┤
              │  ├─ registry   元数据 + 参数 Schema 校验   │
              │  ├─ validator  JSON Schema 判定（fail-closed）
              │  ├─ executor   内置/外部执行 + 读写并发门  │
              │  │     ├─ tool_approval  Cupolas 守卫链 + 审计
              │  │     ├─ interactive    pending / approve 挂起队列
              │  │     └─ os_sandbox     Landlock / seccomp（Linux）
              │  └─ cache      结果缓存（1024 / 3600s）    │
              │                                           ├─ plugin_permission  manifest → 守卫类型
              │                                           ├─ plugin_discovery   扫描 ecosystem/plugins
              │                                           └─ plugin_service     dlopen/dlsym 状态机
              ▼
        builtin{,_fs,_shell,_net,_git,_maths}.c
        maths_eval / maths_stats ──(maths.sock)──▶ maths_d
```

- 服务与执行核心抽为静态库 `airy_tool_service`（`CMakeLists.txt` 中列出 24 个源文件，
  排除 `main.c` / `config.c` / `utils` / `tool_helpers.c`），守护进程与单元测试共用。
- 守护进程在 GNU ld 下以 `-Wl,--whole-archive airy_coreloopthree -Wl,--no-whole-archive`
  链接 atoms 引擎：其适配器经 IPC / LLM / tool ops 表回调（`daemon_ipc_ops_init` /
  `daemon_llm_ops_init` / `daemon_tool_ops_init`），atoms 不直接链接 daemons 符号；
  注入失败非致命，调用点自行降级。
- `concurrent_clients = true`：客户端请求派发到线程池并发处理。否则阻塞在
  `approve` 决策上的 `execute` 会占住事件循环线程，`pending` / `approve`
  请求将无法被处理。

## 内置工具

| 工具 | 门 | 超时 | 缓存 | 参数 |
|------|----|------|------|------|
| `fs_read` | READ | 30s | 否 | `path`(必填) |
| `fs_write` | WRITE | 30s | 否 | `path`(必填)、`content`(必填) |
| `fs_list` | READ | 30s | 是 | `path`（可选，缺省为当前目录） |
| `fs_glob` | READ | 30s | 否 | `pattern`(必填)、`base` |
| `fs_grep` | READ | 60s | 否 | `pattern`(必填)、`path`、`glob`、`max_results` |
| `fs_edit` | WRITE | 30s | 否 | `path`、`old`、`new`(均必填)、`count` |
| `fs_delete` | WRITE | 30s | 否 | `path`(必填)、`recursive`（非空目录须显式置真） |
| `shell_run` | WRITE | 60s | 否 | `command`(必填)、`cwd` |
| `web_fetch` | READ | 45s | 是 | `url`(必填) |
| `web_search` | READ | 45s | 是 | `query`(必填)、`max_results` |
| `git_exec` | READ | 60s | 否 | `command_args`: string[](必填)、`cwd`；仅白名单只读子命令（status / diff / log / branch / show / ls-files / grep） |
| `git_diff` | READ | 60s | 否 | `path`、`staged` |
| `git_apply` | WRITE | 60s | 否 | `patch`(必填)、`check_only` |
| `maths_eval` | READ | 10s | 是 | `expression`(必填，≤4096 字符) |
| `maths_stats` | READ | 10s | 是 | `op`(必填)、`values`: number[](必填) |

`maths_eval` / `maths_stats` 是对 `maths_d` 的转发（经 `<runtime-dir>/maths.sock`，
RPC 超时 3000 ms）。平台限制只落在个别工具上，`tool_d` 自身在 Windows 上完整可用：
Windows 下 `maths_*` 返回明确的不支持错误，`git_*` 三个工具的派发分支不编译（返回
`Unknown builtin tool`），OS 沙箱恒为关闭。

## JSON-RPC 接口

### 工具域（11 个）

由 `main.c` 经 `method_dispatcher_register` 注册：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `register` | `{tool: {id, name, executable, description?, timeout_sec?, cacheable?, permission_rule?, params?: [{name, schema}]}}` | 空 result | 注册外部工具；`id` / `name` / `executable` 缺一返回 `-32602` |
| `list_tools` | `{}` | `[{id, name, description}]` | 列出全部已注册工具 |
| `list` | `{}` | 同 `list_tools` | `list_tools` 的标准名别名 |
| `get_tool` | `{tool_id: string}` | `{id, name, executable, description?, timeout_sec, cacheable, permission_rule?, params?: [{name, schema}]}` | 查询单个工具；未找到 `-32601` |
| `execute_tool` | `{tool_id, params: object, agent_id?}` | `{success, output?, error?, exit_code}` | 执行工具；`agent_id` 缺省时以 `tool_d` 身份判定 ACL |
| `execute` | 同 `execute_tool` | 同 `execute_tool` | `execute_tool` 的标准名别名 |
| `pending` | `{}` | `{pending: [{request_id, tool, agent_id, params, created_at}]}` | 交互式审批挂起队列 |
| `approve` | `{request_id, decision}` | `{resolved: true, request_id, decision}` | `decision` 取 `allow`（单次放行）/ `always`（放行并追加进程内 ACL 授权）/ `deny`（拒绝）；参数缺失或非法、请求不存在均 `-32602` |
| `get_stats` | `{}` | `{daemon: "tool_d", tools, exec_total, exec_fail, exec_ms_total, avg_exec_ms}` | daemon 级执行统计 |
| `health_check` | `{}` | `{service: "tool_d", healthy, timestamp}` | 服务健康检查 |
| `shutdown` | `{}` | — | 优雅退出 |

执行被拒时优先透传执行器的错误描述（如交互式审批拒绝 / 超时的
`User denied tool execution`），便于调用方区分权限拒绝与工具自身失败。

### 插件域（12 个）

由 `plugin_rpc.c` 登记在同一 dispatcher 上，以 `plugin_` 前缀与工具方法区分
（`execute` = 工具执行，`plugin_execute` = 插件执行）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `plugin_load` | `{library_path, config_path?}` | `{name}` | `dlopen` 加载 |
| `plugin_install` | 同 `plugin_load` | 同 `plugin_load` | `plugin_load` 别名 |
| `plugin_unload` | `{name}` | `{unloaded: true}` | 卸载 |
| `plugin_uninstall` | 同 `plugin_unload` | 同 `plugin_unload` | `plugin_unload` 别名 |
| `plugin_start` | `{name}` | `{started: true}` | 启动 |
| `plugin_stop` | `{name}` | `{stopped: true}` | 停止 |
| `plugin_execute` | `{name, input}` | `{output}` | 调用插件导出入口 |
| `plugin_get_metadata` | `{name}` | `{name, version, author, description, type, api_version, min_airy_version}` | 元数据；未找到 `-32601` |
| `plugin_get_state` | `{name}` | `{state}` | 状态机取值 |
| `plugin_get_stats` | `{name?}` | 有 `name`：`{load_count, error_count, uptime_ns, memory_bytes}`；无 `name`：`{daemon: "tool_d", plugins, load_total, error_total, memory_bytes}` | 单插件 / 聚合统计 |
| `plugin_list` | `{type_filter?}` | `{plugins: [string], total}` | 已加载插件列表 |
| `plugin_health_check` | `{}` | `{service: "tool_d", healthy, plugin_count, timestamp}` | 插件域健康检查 |

插件状态机：`UNLOADED → LOADED → INITIALIZED → RUNNING → ERROR/DISABLED`。
权限清单（16 项，`plugin_permission.c`）：`file_read`、`file_write`、
`network_outbound`、`network_inbound`、`tool_execute`、`memory_access`、
`hook_register`、`system_call`、`process_spawn`、`ipc_connect`、
`service_discovery`、`config_read`、`config_write`、`log_write`、
`metrics_export`、`audit_trigger`，各自映射到对应的 SafetyGuard 守卫类型。
严格模式默认开启：未声明、未知或被拒权限的插件一律不加载。

外部调用方使用带命名空间前缀的形式（`tool.execute`、`plugin.load`），由
`gateway_d` 剥离前缀后转发；`plugin.load` / `plugin.unload` / `plugin.install`
在网关侧要求 `cap:plugin.admin`。

## 配置

命令行只有两个选项：`--manager <config>`（配置文件）与 `--tcp`（强制 TCP 监听）。

`--manager` 指向 JSON 文件，`daemon` 段决定端点，缺省值即上文的 socket / 端口：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/tool.sock",
    "tcp_port": 8081,
    "max_clients": 64
  }
}
```

出现 `daemon.tcp_port` 会同时启用 TCP 监听。工具与插件的行为由代码内建的默认值
与下列环境变量决定，没有额外的服务级配置文件。

ACL 双来源（fail-closed：无授权条目即拒绝）：

1. **权威源** `$AIRY_CONFIG_DIR/permission_rules.yaml`，规则形如
   `rules: [{agent: "coding_v1", tool: "fs_read", effect: "allow"}]`；
   文件缺失仅告警，不阻断启动。
2. **环境变量** `AIRY_AGENT_ACL`，格式 `agent=tool1,tool2;agent2=tool3`，
   用于无交互式审批人的服务端预授权。

内置工具注册时同时为 `tool_d` 身份登记 ACL 规则；调用方传入 `agent_id` 时
按真实主体判定。

| 环境变量 | 默认 | 说明 |
|----------|------|------|
| `AIRY_HOME` | `~/.airymaxrt` | 运行时目录根；socket 与插件目录都以此为基准 |
| `AIRY_CONFIG_DIR` | — | `permission_rules.yaml` 所在目录 |
| `AIRY_AGENT_ACL` | 空 | 静态 ACL 预授权 |
| `AIRY_TOOL_SANDBOX_MODE` | `workspace` | `off` / `workspace` / `strict`（`strict` 同时关闭网络） |
| `AIRY_TOOL_SANDBOX_WORKSPACE` | 进程 cwd | 文件与子进程围堵根目录 |
| `AIRY_TOOL_SANDBOX_NET` | 1 | 沙箱内网络访问（`0` / `false` 关闭） |
| `AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK` | 0 | 置 1 时 Landlock 不可用直接失败，而非静默降级 |
| `AIRY_TOOL_APPROVAL_MODE` | 关闭 | 设为 `interactive` 才启用交互式审批 |
| `AIRY_TOOL_APPROVAL_TIMEOUT_MS` | 120000 | 审批等待超时，超时按拒绝处理 |
| `AIRY_TOOL_SOCK` | — | 网关侧覆盖 tool_d 端点 |

沙箱风险声明：默认 `workspace` 模式下，若内核无 Landlock（Linux < 5.13 或裁剪
内核）或平台为非 Linux，沙箱降级为 rlimit + seccomp（非 Linux 完全无 OS 沙箱）
并只输出 WARN 日志——此时 shell 子进程与文件工具失去内核级写保护。安全要求高的
部署请设 `AIRY_TOOL_SANDBOX_REQUIRE_LANDLOCK=1`：`os_sandbox_apply` 直接失败
（fail-closed），拒绝执行而非静默降级。

## 用法

```bash
airymaxrt logs tool_d           # 运行态日志
<build-dir>/bin/tool_d          # 手工启动单个进程（监听默认端点）
```

直连端点排查（绕过 `gateway_d`，方法名不带前缀）：

```bash
printf '%s' '{"jsonrpc":"2.0","id":1,"method":"list_tools","params":{}}' |
  socat - UNIX-CONNECT:"${AIRY_HOME:-$HOME/.airymaxrt}/run/tool.sock"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target tool_d
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

```bash
ctest --test-dir ../daemons-build -R tool_d_ --output-on-failure
```

| ctest 名称 | 覆盖内容 |
|------------|----------|
| `tool_d_test_service` | 服务装配与注册/执行主链路 |
| `tool_d_test_registry` | 注册表与元数据 |
| `tool_d_test_executor` | 执行器与读写并发门 |
| `tool_d_test_validator` | 参数 Schema 校验 |
| `tool_d_test_cache` | 结果缓存命中与过期 |
| `tool_d_test_sandbox_integration` | 沙箱接入执行链路 |
| `tool_d_test_os_sandbox` | Landlock / seccomp 原语（仅 Linux） |
| `tool_d_test_fs_e2e` | 文件工具端到端 |
| `tool_d_test_plugin_permission` | 权限映射与严格模式 |
| `tool_d_test_plugin_discovery` | 离线扩展校验器的 fail-closed 面 |

## 依赖

| 依赖 | 用途 |
|------|------|
| [atoms](https://atomgit.com/openairymax/atoms) | `airy_coreloopthree`（`--whole-archive` 注入 ops 回调）、`airy_syscall`（沙箱 API）、`airy_cognition`、`airy_core` / `airy_memory` / `airy_atoms` / `airy_common` |
| [cupolas](https://atomgit.com/openairymax/cupolas) | SafetyGuard 守卫裁决、审批链与审计 |
| [maths_d](../maths_d/README.md) | `maths_eval` / `maths_stats` 的执行端 |
| [`svc_common`](../common/README.md) | 守护进程样板、事件驱动、JSON-RPC dispatcher、`daemon_security*` ACL |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、内存与同步原语、cJSON 封装 |
| 外部 | `cJSON`、可选 `libyaml`、`Threads::Threads`；Windows 另链 `ws2_32`、`bcrypt`、`${CMAKE_DL_LIBS}` |

## 关系

`tool_d` 只做执行与授权，不做决策与路由：`agent_d` / `think_d` 决定「调哪个工具」，
`gateway_d` 负责命名空间与协议转换，`maths_d` 承担精确数学求值。工具授权以
`cupolas` 的守卫裁决与 `permission_rules.yaml` 为准，跨 daemon 的调用方一律以
`agent_id` 标识主体，权限判定集中在本进程，不在调用方复制。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
