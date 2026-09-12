# hook_d — Hook 事件守护进程

> **模块路径**：`agentrt/daemons/hook_d/` · **可执行文件 / CMake 目标**：`hook_d` · **RPC 命名空间**：`hook.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`hook_d` 把 Hook 事件系统作为独立进程对外服务：注册 / 注销 Hook、按事件类型触发
回调链、返回聚合决策，并维护会话级上下文。Hook 核心（注册表、执行器、拦截器、
超时控制、内置 handler）是独立库 `airy_coreloop_hooks`，`hook_d` 负责把它变成一个
可被跨进程调用的常驻服务。

- 端点：POSIX Unix socket `<runtime-dir>/hook.sock`（`$AIRY_HOME/run/hook.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8093`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8093`（默认只监听 socket）。

## 能力

- **九类事件钩子** — `pre_exec` `post_exec` `pre_llm` `post_llm` `pre_tool`
  `post_tool` `on_error` `on_memory_evolve` `session_start`。
- **四种实现类型** — `shell` / `python` / `webhook` / `callback`；RPC 注册仅接受
  shell / python / webhook（C 回调无法经 RPC 传递），`callback` 只用于内置 handler。
- **聚合决策** — 触发链返回 `decision`：`continue`(0) / `skip` / `retry` / `abort` / `modify`。
- **内置观测 handler** — 启动时统一注册 12 个内置 handler，覆盖指标、审计、追踪三条链路。
- **会话暂存** — `session_start` 触发会话钩子并写入会话暂存，`session_get` 回读上下文。
- **真实统计** — 单个 Hook 的调用、跳过、中止、重试、改写次数与耗时均计入统计。

## 架构

```
gateway_d ──(hook.* JSON-RPC)──▶ hook_d（进程入口：socket + 服务发现 + IPC + 安全引导）
                                   │ 链接 airy_coreloop_hooks
                                   ▼
                    Hook 系统核心（atoms/coreloopthree/src/hook/）
                      ├─ hook_registry     按事件类型分组的注册表
                      ├─ hook_service      触发链聚合与决策
                      ├─ hook_executor     执行器（shell / python / webhook / callback）
                      ├─ hook_interceptor  拦截器
                      ├─ hook_timeout      超时控制
                      └─ hook_builtin_handlers  统一注册入口（12 个内置 handler）
```

内置 handler 构成：metrics 8（覆盖全部事件类型，priority 50）+ audit 2
（`on_error`、`post_tool`，priority 80）+ trace 2（`pre_exec`、`post_exec`，
priority 90 / 10）。

## JSON-RPC 接口

共 13 个方法，经 `method_dispatcher_register` 注册（方法名不含命名空间前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `register` | `{name, type: string\|int, impl?: shell\|python\|webhook\|callback, script_path?, priority?: int, enabled?: bool}` | `{status: "registered", name, type, enabled}` | 注册 Hook；重名或注册表满返回 `-32603` |
| `unregister` | `{name}` | `{status: "unregistered", name}` | 注销 Hook；未找到返回 `-32601` |
| `trigger` | `{type: string\|int, operation?, input?, hook_name?}` | `{decision: int, decision_name, type}` | 触发指定类型的 Hook 链并返回聚合决策 |
| `session_start` | `{session_id, operation?, input?}` | `{session_id, decision, decision_name}` | 触发 `session_start` 链并写入会话暂存 |
| `session_get` | `{session_id}` | `{session_id, active, started_at?, decision?, hook_count?, injected_context?}` | 读取会话暂存 |
| `list` | `{}` | `{hooks: [{name, type, type_id, impl_type, priority, enabled, invoke_count, skip_count, abort_count, total_duration_ns, script_path?}...], count}` | 列出已注册 Hook 及统计 |
| `status` | `{}` | `{service, hook_count, registry_initialized, by_type: {...}}` | 总数与各类型计数 |
| `stats` | `{name}` | `{name, invoke_count, skip_count, abort_count, retry_count, modify_count, total_duration_ns, max_duration_ns}` | 单个 Hook 的统计 |
| `ping` | `{}` | `{status: "ok", uptime_sec}` | 存活探测 |
| `health` | `{}` | `{healthy: bool, hook_count}` | 注册表健康状态 |
| `health_check` | `{}` | `{service, healthy, hook_count, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, hooks, uptime_s}` | daemon 级统计 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`hook.register`），由 `gateway_d` 剥离前缀后转发。

## 配置

- 命令行参数：`--manager <config>`、`--tcp`、`--help`。
- 事件驱动：线程池 2~4，队列 128，`max_events=64`。
- 环境变量：`AIRY_HOOK_SOCK` 用于在网关侧覆盖本进程端点。

## 用法

```bash
airymaxrt logs hook_d         # 运行态日志
<build-dir>/bin/hook_d        # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target hook_d
ctest --test-dir ../daemons-build -R "hook_d_" --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 依赖

| 依赖 | 用途 |
|------|------|
| [airy_coreloop_hooks](https://atomgit.com/openairymax/atoms) | Hook 注册表、执行器、拦截器、超时、内置 handler |
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 安全裁决与审计 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON 封装 |
| [corekern](https://atomgit.com/openairymax/atoms) | `airy_init()` 核心引导 |
| libcurl / libyaml | 可选：webhook 实现、YAML 配置 |
| [gateway_d](../gateway_d/README.md) | 上游：`hook.*` 命名空间转发方 |

## 关系

Hook 核心以库形态存在，`hook_d` 是它的**进程出入口**：同进程内的 Agent 执行循环直接
调用 `airy_coreloop_hooks`，跨进程的注册与触发走 `hook_d`。`tool_d`、`llm_d`、`agent_d`
的执行前后事件即对应这里的 `pre_tool` / `post_tool` / `pre_llm` / `post_llm` 类型。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
