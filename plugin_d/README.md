# Plugin Daemon — 插件管理守护进程

> **模块路径**: `agentrt/daemons/plugin_d/` | **版本**: v0.1.1

## 概述

`daemons/plugin_d/` 是 AgentRT 十二大运行时守护进程之一，负责动态插件的发现、加载、权限校验、生命周期管理和沙箱隔离。它支持四种插件类型（工具提供者 / 协议适配器 / 记忆提供商 / Hook 扩展），通过 `manifest.yaml` 声明式管理插件元数据，并与 Cupolas 安全穹顶深度集成，确保每个插件在加载前都经过严格的权限校验。

### 架构定位

```
plugin_d → plugin_discovery → plugin_permission → plugin_service → cupolas (安全穹顶)
    ↑            ↑                    ↑                ↑
 守护进程    manifest.yaml        Cupolas 守卫      动态库加载
 生命周期    目录扫描              权限映射          dlopen/dlsym
```

### 核心职责

- **插件发现（Discovery）**：扫描 `ecosystem/plugins/` 目录，解析 `manifest.yaml` 发现可用插件
- **权限校验（Permission）**：将 manifest 中声明的权限映射到 Cupolas 安全穹顶守卫类型，逐项校验
- **动态加载（Load）**：通过 `dlopen` 加载插件动态库，调用 `plugin_init` 入口点
- **生命周期管理**：插件状态机（UNLOADED → LOADED → INITIALIZED → RUNNING → ERROR / DISABLED）
- **自动启动**：权限校验通过后自动加载并启动插件
- **服务发现注册**：通过 ServiceDiscovery 和 IPC Bus 在运行时注册自身服务

## 目录结构

```
plugin_d/
├── CMakeLists.txt              # 构建配置（4 个源文件）
├── README.md                   # 本文件
├── include/                    # 公共头文件
│   ├── plugin_service.h        # 插件服务 API（加载/卸载/启动/停止/元数据/统计）
│   ├── plugin_discovery.h      # 插件发现（目录扫描 + manifest.yaml 解析）
│   └── plugin_permission.h     # 插件权限校验（manifest 权限 ↔ Cupolas 守卫映射）
├── src/                        # 实现文件
│   ├── main.c                  # 守护进程入口（P2.2 完整实现）
│   ├── service.c               # 插件服务核心实现（加载/卸载/状态机）
│   ├── plugin_discovery.c      # 插件发现实现（目录扫描/manifest 解析/自动加载）
│   └── plugin_permission.c     # 权限校验实现（8 种守卫类型映射）
└── tests/
    └── CMakeLists.txt          # 单元测试构建配置
```

## 核心组件说明

### 插件类型

| 类型 | 枚举值 | 说明 |
|------|--------|------|
| **工具提供者** | `PLUGIN_TYPE_TOOL_PROVIDER` | 提供外部工具能力（如 web_search / code_exec / file_ops） |
| **协议适配器** | `PLUGIN_TYPE_PROTOCOL_ADAPTER` | 协议转换适配（如自定义 RPC 协议 → JSON-RPC 2.0） |
| **记忆提供商** | `PLUGIN_TYPE_MEMORY_PROVIDER` | 记忆存储后端（如向量数据库 / 知识图谱） |
| **Hook 扩展** | `PLUGIN_TYPE_HOOK_EXTENSION` | Hook 事件扩展（如自定义审计/日志处理器） |

### 插件状态机

```
UNLOADED → LOADED → INITIALIZED → RUNNING
    ↓         ↓          ↓            ↓
    └─────────┴──────────┴────→ ERROR / DISABLED
```

| 状态 | 枚举值 | 说明 |
|------|--------|------|
| UNLOADED | `PLUGIN_STATE_UNLOADED` | 未加载 |
| LOADED | `PLUGIN_STATE_LOADED` | 已加载（dlopen 成功） |
| INITIALIZED | `PLUGIN_STATE_INITIALIZED` | 已初始化（plugin_init 回调成功） |
| RUNNING | `PLUGIN_STATE_RUNNING` | 运行中（plugin_start 回调成功） |
| ERROR | `PLUGIN_STATE_ERROR` | 错误状态 |
| DISABLED | `PLUGIN_STATE_DISABLED` | 已禁用 |

### 插件入口点回调

每个插件动态库必须导出四个标准回调函数：

| 回调 | 签名 | 说明 |
|------|------|------|
| `plugin_init` | `int (*)(const char *config_path, void **user_data)` | 初始化：读取配置，分配资源 |
| `plugin_destroy` | `void (*)(void *user_data)` | 销毁：释放资源 |
| `plugin_start` | `int (*)(void *user_data)` | 启动：开始服务 |
| `plugin_stop` | `int (*)(void *user_data)` | 停止：暂停服务 |

### manifest.yaml 格式

每个插件在 `ecosystem/plugins/<name>/manifest.yaml` 中声明元数据：

```yaml
name: my_plugin
version: 1.0.0
author: SPHARX
description: My plugin description
type: tool_provider          # tool_provider | protocol_adapter | memory_provider | hook_extension
api_version: 1
min_airy_version: 0.1.1
library: libmy_plugin.so
permissions:                 # 权限声明，与 Cupolas 守卫映射
  - file_read
  - file_write
  - network_outbound
  - tool_execute
  - memory_access
  - hook_register
  - system_call
  - process_spawn
config:
  timeout_ms: 5000
```

### 权限映射（manifest → Cupolas）

| manifest 权限 | Cupolas 守卫类型 | 说明 |
|---------------|-----------------|------|
| `file_read` | `SAFETY_GUARD_FILE_READ` | 文件读取 |
| `file_write` | `SAFETY_GUARD_FILE_WRITE` | 文件写入 |
| `network_outbound` | `SAFETY_GUARD_NETWORK` | 网络出站 |
| `tool_execute` | `SAFETY_GUARD_TOOL_EXEC` | 工具执行 |
| `memory_access` | `SAFETY_GUARD_MEMORY` | 内存访问 |
| `hook_register` | `SAFETY_GUARD_HOOK` | Hook 注册 |
| `system_call` | `SAFETY_GUARD_SYSTEM` | 系统调用 |
| `process_spawn` | `SAFETY_GUARD_PROCESS` | 进程派生 |

### 启动流程

```
1. 信号处理注册 (SIGINT/SIGTERM → graceful shutdown)
2. Cupolas 安全穹顶初始化 (daemon_cupolas_init)
3. Unix Socket 服务器创建 (AIRY_RUNTIME_DIR/plugin.sock)
4. ServiceDiscovery 自动注册 (tag: plugin,core)
5. IPC Bus 消息路由注册 (JSON-RPC 2.0)
6. 权限校验模块初始化 (strict_mode + audit_log)
7. 插件发现模块初始化 (扫描 ecosystem/plugins/)
8. 扫描插件目录，解析 manifest.yaml
9. 对每个发现的插件：权限校验 → 加载 → 自动启动
10. 进入事件循环，等待 shutdown 信号
```

## 上游依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| **svc_common** | `agentrt/daemons/common/` | 守护进程框架（ServiceDiscovery / IPC Bus / Cupolas bootstrap / 日志） |
| **cupolas** | `agentrt/cupolas/` | 安全穹顶（permission_engine + safety_guard），提供守卫类型枚举与权限裁决 |
| **commons** | `agentrt/commons/` | 基础库（logging / config_unified / memory / sync / string / error） |
| **airy_common** | `agentrt/commons/` | 通用运行时库 |
| libdl | 系统 | 动态库加载（dlopen / dlsym / dlclose） |
| libyaml | 外部 | manifest.yaml 解析 |
| cJSON | 外部 | JSON 配置解析 |

## 下游消费者

| 消费者 | 使用方式 |
|--------|----------|
| **market_d** | 通过 IPC Bus 调用 plugin_service API 管理插件安装/卸载 |
| **tool_d** | 通过 plugin_d 加载工具提供者插件，扩展工具执行能力 |
| **gateway_d** | 通过 plugin_d 加载协议适配器插件，扩展协议转换能力 |
| **Agent 开发者** | 编写符合 manifest.yaml 规范的插件动态库，放入 `ecosystem/plugins/` |

## 构建

### 前置条件

- CMake ≥ 3.16
- C11 编译器（GCC / Clang / MSVC）
- `svc_common` 已构建（daemons/common 子项目）
- `cupolas` 已构建
- libdl（Linux）/ 系统动态库加载支持

### 编译选项

```bash
# 标准构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target plugin_d

# 启用测试
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target plugin_d
ctest --test-dir build

# 启用覆盖率
cmake -S . -B build -DBUILD_COVERAGE=ON
cmake --build build --target plugin_d
```

## 运行时

### 启动

```bash
# 直接启动
./build/bin/plugin_d

# 通过 daemon_manager 启动
./daemon_manager --start plugin_d
```

### Unix Socket

- 路径：`${AIRY_RUNTIME_DIR}/plugin.sock`（默认 `/var/run/agentrt/plugin.sock`）
- 协议：JSON-RPC 2.0 over Unix Socket

### 服务 API

| API | 说明 |
|-----|------|
| `plugin_service_load` | 从动态库加载插件 |
| `plugin_service_unload` | 卸载插件 |
| `plugin_service_start` | 启动插件 |
| `plugin_service_stop` | 停止插件 |
| `plugin_service_get_metadata` | 获取插件元数据 |
| `plugin_service_get_state` | 获取插件状态 |
| `plugin_service_get_stats` | 获取插件统计（加载次数/错误次数/运行时间/内存） |
| `plugin_service_list` | 列出所有已加载插件（可按类型过滤） |

## 设计原则

- **声明式管理**：通过 `manifest.yaml` 声明插件元数据和权限，避免硬编码
- **安全优先**：加载前强制权限校验，严格模式（`strict_mode`）拒绝未声明权限
- **可插拔架构**：插件独立动态库，支持热加载/热卸载
- **内生安全**：通过 Cupolas 安全穹顶进行权限裁决，审计日志记录所有操作

## 许可证

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

双许可证：**AGPL-3.0-or-later OR Apache-2.0**（SPDX: `AGPL-3.0-or-later OR Apache-2.0`）。详见 [LICENSE](../../LICENSE)。

---

> **文档结束** | 0.1.1（P2.2 完整实现：插件发现 + 权限校验 + 动态加载 + 生命周期管理）