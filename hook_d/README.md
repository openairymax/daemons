# Hook Daemon — Hook 事件注入守护进程

> **模块路径**: `agentrt/daemons/hook_d/`

## 定位

`hook_d` 是 AgentRT 的 Hook 事件注入守护进程。M3（0.1.9 §4.2-2）后，Hook 系统核心
（hook_registry / executor / interceptor / timeout / handlers，位于
`atoms/coreloopthree/src/hook/`）已拆独立库 `airy_coreloop_hooks`，`hook_d` 退化为仅含
`main.c` 的薄 daemon 壳，通过链接 `airy_coreloop_hooks` 获得 Hook 能力（不再链接整个
认知引擎 `airy_coreloopthree`——消费者减重），负责 socket + 服务发现 +
IPC + cupolas 守护进程生命周期，并对外暴露 `hook.*` JSON-RPC 方法族（L2 服务协议）。

## 架构

```
gateway_d ──(hook.* JSON-RPC)──▶ hook_d（薄 daemon 壳）
                                   │ 链接 airy_coreloop_hooks（独立小库）
                                   ▼
                  atoms/coreloopthree/src/hook/（hook 系统核心，编译入 airy_coreloop_hooks）
                    ├─ hook_registry     注册表（按类型分组，上限 HOOK_REGISTRY_MAX）
                    ├─ hook_service      触发链聚合（hook_service_fire 决策）
                    ├─ hook_executor     执行器（shell/python/webhook/callback）
                    ├─ hook_interceptor  拦截器
                    ├─ hook_timeout      超时控制
                    ├─ hook_audit_handler / hook_metrics_handler / hook_trace_handler
                    └─ hook_builtin_handlers（统一注册入口，共 13 个内置 handler）
```

- Hook 类型（9 种）：`pre_exec` `post_exec` `pre_llm` `post_llm` `pre_tool`
  `post_tool` `on_error` `on_memory_evolve` `session_start`（P1-5 会话启动注入）。
- 实现类型：`shell` / `python` / `webhook` / `callback`。RPC 注册仅支持
  shell/python/webhook（RPC 无法传递 C 回调），`callback` 类型限内置 handler。
- 触发返回聚合决策 `decision`：`continue`(0) / `skip` / `retry` / `abort` / `modify`。

## JSON-RPC 接口表

监听端点：Unix socket `$AIRY_RUNTIME_DIR/hook.sock`（`--tcp` 或 Windows 下为
TCP `127.0.0.1:8093`，Windows pipe `\\.\pipe\airy_hook`）。以下方法由
`method_dispatcher_register` 实际注册（main.c），共 11 个：

| 方法 | 参数 | 返回要点 | 说明 |
|------|------|----------|------|
| `health` | 无 | `{"healthy":bool,"hook_count":N}` | 注册表健康 + 已注册 Hook 总数 |
| `ping` | 无 | `{"status":"ok","uptime_sec":N}` | 存活探测（含运行时长） |
| `status` | 无 | `{"service":"hook_d","hook_count","registry_initialized","by_type":{...}}` | 真实状态：总数 + 各类型计数 |
| `list` | 无 | `{"hooks":[{name,type,type_id,impl_type,priority,enabled,invoke_count,skip_count,abort_count,total_duration_ns,script_path?},...],"count":N}` | 列出已注册（enabled）Hook 及统计 |
| `stats` | `name`（必填） | `{name,invoke_count,skip_count,abort_count,retry_count,modify_count,total_duration_ns,max_duration_ns}` | 单个 Hook 的统计 |
| `register` | `name`（必填）、`type`（字符串或 int）、`impl`（shell/python/webhook/callback，默认 shell）、`script_path`、`priority`（默认 0）、`enabled`（默认 true） | `{"status":"registered","name","type","enabled"}` | 注册 script/webhook 类型 Hook；重名 -32603，注册表满 -32603 |
| `unregister` | `name`（必填） | `{"status":"unregistered","name"}` | 注销 Hook；未找到 -32601 |
| `trigger` | `type`（必填，字符串或 int）、`operation`（可选）、`input`（可选）、`hook_name`（可选） | `{"decision":N,"decision_name":"continue\|skip\|retry\|abort\|modify","type"}` | 触发指定类型 Hook 链并返回聚合决策 |
| `health_check` | 无 | `{"service":"hook_d","healthy","hook_count","timestamp"}` | L2 标准方法 |
| `shutdown` | 无 | — | L2 标准方法，触发优雅退出 |
| `get_stats` | 无 | `{"daemon":"hook_d","hooks":N,"uptime_s":N}` | daemon 级统计 |

- 内置 handler 共 13 个：metrics 9（全部事件类型含 session_start，priority=50）+
  audit 2（`on_error` + `post_tool`，priority=80）+ trace 2（`pre_exec` + `post_exec`，
  priority=90/10）；启动时经 `airy_hook_register_builtin_handlers()` 注册，
  `status`/`list` 返回真实已加载模块信息。

## 配置

- 默认配置路径：由 `daemon_parse_args` 解析（`--manager <config>` / `-c`），当前
  `config_path` 未参与 hook 逻辑（daemon 配置以内置默认 + 环境变量为主）。
- 命令行参数：`--manager <config>` / `-c <config>`、`--tcp`（TCP 回环，Windows 强制）、
  `-h` / `--help`。
- 事件驱动：线程池 2~4，队列 128，`max_events=64`。

## 依赖与构建

- 依赖：`airy_coreloop_hooks`（hook 系统核心独立库，GNU ld 下 `--start-group/--end-group`）、
  `svc_common`、`cupolas`、`Threads::Threads`；可选 `YAML`（libyaml）、`CURL`（webhook 实现）。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target hook_d
```

## 测试

- `tests/test_hook_daemon.c`（`hook_d_test_hook_daemon`）：hook_d JSON-RPC 冒烟测试
  （按 `TEST_BIN_DIR` 定位二进制，覆盖 health/ping/status/list/stats/register/
  unregister/trigger/health_check/get_stats 等方法的真实往返）。
- 运行：

```bash
ctest --test-dir build -R "hook_d_" -V
```
