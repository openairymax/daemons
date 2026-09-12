# market_d — 应用市场守护进程

> **模块路径**：`agentrt/daemons/market_d/` · **可执行文件 / CMake 目标**：`market_d` · **RPC 命名空间**：`market.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`market_d` 是 AgentRT 的 Agent 与 Skill 资源市场：负责两类资源的注册、搜索、安装与
发布，是 Agent / Skill 从打包到落地的流转节点。默认数据落盘在 `$AIRY_HOME/agents`
与 `$AIRY_HOME/skills`，远程注册中心同步默认关闭。

- 端点：POSIX Unix socket `<runtime-dir>/market.sock`（`$AIRY_HOME/run/market.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8082`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8082`（默认只监听 socket）。

## 能力

- **双资源注册表** — Agent 与 Skill 各自独立注册、搜索、列举。
- **安装器** — 支持指定版本、安装路径与强制更新；返回实际安装版本与路径。
- **发布** — 接受完整的 `agent` 或 `skill` 描述对象，持久化到本地仓库并返回安装路径。
- **别名方法** — `search` / `install` 分别是 `search_agents` / `install_agent` 的
  标准别名，便于统一编排。
- **可安装状态** — 搜索结果中的 `installed` 字段来自注册表状态，不是启发式推断。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket / TCP)
        ↓
  main.c（accept 循环 + 线程池 4~8 / 队列 256 + 方法分发）
        ↓
  market_service（注册 / 搜索 / 安装 / 发布）
        ├── market_service_registry.c   Agent 与 Skill 注册表
        ├── market_service_search.c     关键词搜索与分页
        ├── market_service_install.c    安装器（版本 / 强制更新 / 安装路径）
        ├── market_service_listing.c    列举与计数
        ├── publisher.c                 资源发布
        └── market_service_config.c     配置装配
```

- 启动时使用内建默认配置：`registry_url = NULL`、`storage_path = NULL`
  （由服务层回退到 `$AIRY_HOME/agents` 或 `$AIRY_HOME/skills`）、
  `sync_interval_ms = 30000`、`cache_ttl_ms = 3600000`、
  `enable_remote_registry = false`、`enable_auto_update = false`；
- 服务发现标签 `market,core`，随进程注册到 `svc_common` 的服务发现后端。

## JSON-RPC 接口

共 11 个方法，经 `method_dispatcher_register` 注册（方法名不含命名空间前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `register_agent` | `{agent: {agent_id, name?, version?, description?, author?}}` | `{status: "registered", agent_id}` | 注册 Agent |
| `search_agents` | `{keyword?: string, offset?: int, limit?: int}`（默认 `""` / 0 / 20） | Agent 数组（含 `installed` 布尔） | 搜索 Agent |
| `install_agent` | `{agent_id, version?: "latest", install_path?, force_update?}` | `{status, agent_id, installed_version, message?, install_path?}` | 安装 Agent |
| `register_skill` | `{skill: {skill_id, name?, version?}}` | `{status: "registered", skill_id}` | 注册 Skill |
| `search_skills` | `{keyword?: string}` | Skill 数组（`limit` 固定 20、`offset` 0） | 搜索 Skill |
| `publish` | `{agent \| skill: object, version?: "latest", install_path?, force_update?}` | `{status: "published", type, id, published_version, message?, install_path?}` | 发布（落盘）Agent 或 Skill |
| `search` | 同 `search_agents` | 同 `search_agents` | `search_agents` 别名 |
| `install` | 同 `install_agent` | 同 `install_agent` | `install_agent` 别名 |
| `health_check` | `{}` | `{service, healthy, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, agents, skills, installed_agents, installed_skills}` | 注册与安装计数 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`market.install`），由 `gateway_d` 剥离前缀后转发。

## 配置

`market_config_t` 字段：`registry_url`、`storage_path`、`sync_interval_ms`、
`cache_ttl_ms`、`enable_remote_registry`、`enable_auto_update`。进程启动时使用内建
默认值，本模块不随附默认配置文件。

命令行参数：`--manager <config>`、`--tcp`、`--help`。

环境变量：`AIRY_MARKET_SOCK` 用于在网关侧覆盖本进程端点。

## 用法

```bash
airymaxrt logs market_d       # 运行态日志
<build-dir>/bin/market_d      # 手工启动单个进程（监听运行目录）
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target market_d
ctest --test-dir ../daemons-build -R "market_d_" --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher、服务发现 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON 封装、缓存 |
| [corekern](https://atomgit.com/openairymax/atoms) | `airy_init()` 核心引导 |
| [gateway_d](../gateway_d/README.md) | 上游：`market.*` 命名空间转发方 |

## 关系

`market_d` 只管资源目录与安装落地；Agent 的实际运行生命周期在 `agent_d`，
可执行工具包由 `tool_d` 装载。三者以文件系统上的 `$AIRY_HOME` 目录为交接点，
不存在进程间直接调用。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
