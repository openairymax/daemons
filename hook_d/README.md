# Hook Daemon — Hook 事件注入守护进程

> **模块路径**: `agentrt/daemons/hook_d/` | **版本**: v0.1.1

## 概述

`daemons/hook_d/` 是 AgentRT 十二大运行时守护进程之一，负责 Hook 事件系统的守护进程生命周期管理。在 SP04 重构后，Hook 系统核心（hook_registry / executor / interceptor / timeout / handlers，共 9 个 .c + 7 个 .h）已迁移至 `atoms/coreloopthree/src/hook/`，`hook_d` 退化为仅含 `main.c` 的薄 daemon 壳，通过链接 `agentrt_coreloopthree` 获取 Hook 系统全部能力。

### 架构定位

```
hook_d (daemon shell) → agentrt_coreloopthree (hook 系统核心) → cupolas (安全穹顶)
         ↑                              ↑
    守护进程生命周期              Hook 注册/执行/拦截/超时
    (socket + SD + IPC)          (9 个 .c + 7 个 .h)
```

### 核心职责

- **Unix Socket 服务**：在 `AGENTRT_RUNTIME_DIR/hook.sock` 上监听 Unix Socket，接收来自 `sched_d` 和 `tool_d` 的 Hook 注入请求
- **ServiceDiscovery 注册**：通过 `daemon_bootstrap_sd` 在服务发现总线上注册自身，注册 tag 为 `hook,core`
- **IPC Bus 消息路由**：通过 `daemon_bootstrap_ipc` 接入 JSON-RPC 2.0 统一 IPC 服务总线
- **Cupolas 安全穹顶集成**：启动时初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger），继承内生安全能力
- **跨平台信号处理**：Linux 支持 SIGINT/SIGTERM/SIGPIPE/SIGUSR1（日志级别热切换），Windows 支持控制台事件处理

## 目录结构

```
hook_d/
├── CMakeLists.txt       # 构建配置（仅编译 main.c，链接 agentrt_coreloopthree）
├── README.md            # 本文件
├── src/
│   └── main.c           # 守护进程入口（薄 daemon 壳）
└── tests/
    └── CMakeLists.txt   # 单元测试构建配置
```

## 核心组件说明

### 启动流程

```
1. 信号处理注册 (SIGINT/SIGTERM → graceful shutdown)
2. Cupolas 安全穹顶初始化 (daemon_cupolas_init)
3. Unix Socket 服务器创建 (AGENTRT_RUNTIME_DIR/hook.sock)
4. ServiceDiscovery 自动注册 (daemon_bootstrap_sd_start)
5. IPC Bus 消息路由注册 (daemon_bootstrap_ipc_start)
6. 进入事件循环，等待 shutdown 信号
7. 优雅停机：清理 IPC → SD → socket → cupolas
```

### Hook 系统核心（位于 atoms/coreloopthree）

Hook 系统核心在 `atoms/coreloopthree/src/hook/` 中实现，`hook_d` 通过 `target_link_libraries(hook_d PRIVATE agentrt_coreloopthree)` 获取。核心组件包括：

| 组件 | 说明 |
|------|------|
| **hook_registry** | Hook 注册表，管理 Hook 的注册、注销和查找 |
| **hook_executor** | Hook 执行器，按优先级顺序执行注册的 Hook 链 |
| **hook_interceptor** | Hook 拦截器，在关键事件点（任务提交/执行/完成）触发 Hook |
| **hook_timeout** | Hook 超时管理，防止单个 Hook 阻塞整个执行链 |
| **hook_handlers** | 内置 Hook 处理器集合（日志/审计/安全/监控） |

### 守护进程生命周期

`hook_d` 本身不包含 Hook 业务逻辑，仅负责守护进程生命周期管理：

| 阶段 | 操作 | 说明 |
|------|------|------|
| 启动 | `daemon_cupolas_init` | 初始化安全穹顶 |
| 启动 | `agentrt_socket_create_unix_server` | 创建 Unix Socket |
| 注册 | `daemon_bootstrap_sd_start` | 向 ServiceDiscovery 注册 |
| 注册 | `daemon_bootstrap_ipc_start` | 向 IPC Bus 注册 |
| 运行 | `sleep(1)` 循环 | 等待关闭信号 |
| 停止 | `daemon_bootstrap_ipc_stop` | 注销 IPC 路由 |
| 停止 | `daemon_bootstrap_sd_stop` | 注销服务发现 |
| 停止 | `agentrt_socket_close` | 关闭 Socket |
| 清理 | `daemon_cupolas_cleanup` | 清理安全穹顶 |

## 上游依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| **agentrt_coreloopthree** | `agentrt/atoms/coreloopthree/` | Hook 系统核心（registry / executor / interceptor / timeout / handlers） |
| **svc_common** | `agentrt/daemons/common/` | 守护进程框架（ServiceDiscovery / IPC Bus / Cupolas bootstrap / 日志 / 配置） |
| **cupolas** | `agentrt/cupolas/` | 安全穹顶（permission_engine + sanitizer + audit_logger），通过 `daemon_cupolas_bootstrap` 集成 |
| **commons** | `agentrt/commons/` | 基础库（logging / config_unified / network / memory / sync / string / cache / compat / error / ipc） |
| cJSON / libcurl / libyaml | 外部 | JSON 解析 / HTTP 客户端 / YAML 配置 |

## 下游消费者

| 消费者 | 使用方式 |
|--------|----------|
| **sched_d** | 在任务调度关键节点（任务提交/分配/完成）通过 Unix Socket 注入 Hook |
| **tool_d** | 在工具执行前后通过 Unix Socket 注入 Hook（审计/日志/安全校验） |
| **其他 daemon** | 通过 IPC Bus 请求 Hook 注入服务 |

## 构建

### 前置条件

- CMake ≥ 3.16
- C11 编译器（GCC / Clang / MSVC）
- `agentrt_coreloopthree` 已构建（atoms 子项目）
- `svc_common` 已构建（daemons/common 子项目）

### 编译选项

```bash
# 标准构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hook_d

# 启用测试
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target hook_d
ctest --test-dir build

# 启用覆盖率
cmake -S . -B build -DBUILD_COVERAGE=ON
cmake --build build --target hook_d
```

### 链接策略

Linux 平台使用 `--start-group/--end-group` 避免 LTO + ASan 下符号扫描遗漏：

```cmake
target_link_libraries(hook_d PRIVATE
    "-Wl,--start-group"
    agentrt_coreloopthree
    svc_common
    cupolas
    "-Wl,--end-group"
    Threads::Threads
)
```

## 运行时

### 启动

```bash
# 直接启动
./build/bin/hook_d

# 通过 daemon_manager 启动
./daemon_manager --start hook_d
```

### Unix Socket

- 路径：`${AGENTRT_RUNTIME_DIR}/hook.sock`（默认 `/var/run/agentrt/hook.sock`）
- 协议：JSON-RPC 2.0 over Unix Socket

### 信号

| 信号 | 平台 | 行为 |
|------|------|------|
| SIGINT | Linux | 优雅停机 |
| SIGTERM | Linux | 优雅停机 |
| SIGPIPE | Linux | 忽略（Socket 层独立处理） |
| SIGUSR1 | Linux | 日志级别热切换（INFO ↔ DEBUG） |
| CTRL_C_EVENT | Windows | 优雅停机 |
| CTRL_CLOSE_EVENT | Windows | 优雅停机 |

## 设计原则

- **薄 daemon 壳**：Hook 业务逻辑位于 `atoms/coreloopthree`，`hook_d` 仅负责进程生命周期
- **单一职责**：每个 daemon 独立进程，通过 IPC 协作
- **内生安全**：通过 `svc_common` 继承 Cupolas 安全穹顶能力
- **跨平台**：Linux 和 Windows 双平台支持，通过 `daemon_platform_ext.h` 适配

## 许可证

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

双许可证：**AGPL-3.0-or-later OR Apache-2.0**（SPDX: `AGPL-3.0-or-later OR Apache-2.0`）。详见 [LICENSE](../../LICENSE)。

---

> **文档结束** | 0.1.1（SP04 重构后：Hook 系统核心已迁移至 atoms/coreloopthree，hook_d 退化为薄 daemon 壳）