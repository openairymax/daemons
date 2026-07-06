**语言:** [English](README.md) | 简体中文

# Airymax Daemons — 运行时守护进程服务

`agentrt/daemons/`

**版本：** 0.1.1
**许可证：** AGPL-3.0-or-later OR Apache-2.0（双许可证）
**分支：** `feature/official-hubs-01`

---

## 1. 模块定位

Daemons 是 Airymax 智能体运行时的**用户态服务层**。它由 **12 个独立守护进程**
组成，共同为智能体系统提供完整的后端服务支撑。每个守护进程遵循**职责单一原则**：
独立进程运行，通过统一 IPC 服务总线协作通信，构成位于 Airymax 内核之上的
高可用、可扩展、可插拔微服务架构。

设计目标：

- **服务化架构** —— 每个守护进程独立运行，通过 IPC 协作，支持独立部署与扩缩容。
- **职责单一** —— 每个守护进程只负责一个核心领域，降低耦合度。
- **可插拔** —— 守护进程可独立部署、升级和替换，不影响其他服务。
- **高可用** —— 支持主备切换、熔断器保护、故障转移和自动恢复。
- **安全内生** —— 每个守护进程链接 `svc_common`，从而传递链接 Cupolas；
  所有请求必须在零信任模型下通过鉴权。
- **协议统一** —— 所有守护进程通过 JSON-RPC 2.0 通信，网关层支持
  MCP / A2A / OpenAI API 协议转换。

---

## 2. 目录结构

```
daemons/
├── CMakeLists.txt                 # 顶层构建文件，管理所有子模块
├── Dockerfile.ci                  # CI 环境 Docker 镜像
├── README.md                      # 英文版
├── README_zh.md                   # 本文件（中文）
├── LICENSE                        # 双许可证文本（AGPL-3.0 + Apache-2.0）
├── NOTICE                         # 版权声明
├── common/                        # 公共服务库（svc_common，18+ 组件）
├── gateway_d/                     # API 网关守护进程
├── llm_d/                         # LLM 服务守护进程
├── tool_d/                        # 工具执行守护进程
├── sched_d/                       # 任务调度守护进程
├── market_d/                      # 应用市场守护进程
├── monit_d/                       # 监控告警守护进程
├── channel_d/                     # 通信通道守护进程
├── info_d/                        # 信息服务守护进程
├── notify_d/                      # 通知推送守护进程
├── observe_d/                     # 观测服务（OpenTelemetry）守护进程
├── hook_d/                        # Hook 守护进程（薄壳；核心在 atoms/coreloopthree）
├── plugin_d/                      # Plugin 守护进程
├── examples/                      # 使用示例（example_svc_usage.c）
└── scripts/                       # 构建/CI/分析脚本
    ├── ci.sh
    ├── local-ci.sh
    ├── static-analysis.sh
    └── verify-coverage.sh
```

### 12 个守护进程

| 守护进程 | 目录 | 职责 | CMake Target |
|----------|------|------|--------------|
| **API 网关** | `gateway_d/` | 外部请求接入，协议转换（HTTP/WS/MCP/A2A/OpenAI API）与路由 | `gateway_d` |
| **LLM 服务** | `llm_d/` | 大语言模型调用、Token 计数、成本追踪、响应缓存 | `llm_d` |
| **工具执行** | `tool_d/` | 工具注册/发现、沙箱执行、参数校验、结果缓存 | `tool_d` |
| **任务调度** | `sched_d/` | 任务分发，4 种调度策略（轮询/加权/优先级/ML） | `sched_d` |
| **应用市场** | `market_d/` | Agent/Skill/Tool/Template 资源管理、安装、版本控制 | `market_d` |
| **监控告警** | `monit_d/` | 指标采集、健康检查、告警管理、Agent 死循环检测 | `monit_d` |
| **通道服务** | `channel_d/` | 通信通道管理与消息路由 | `channel_d` |
| **信息服务** | `info_d/` | 系统信息查询与状态报告 | `info_d` |
| **通知服务** | `notify_d/` | 多渠道通知推送（邮件/Slack/Discord） | `notify_d` |
| **观测服务** | `observe_d/` | OpenTelemetry 可观测性数据采集 | `observe_d` |
| **Hook 守护进程** | `hook_d/` | 薄守护进程壳；Hook 系统核心位于 `atoms/coreloopthree/src/hook/`，通过链接 `agentrt_coreloopthree` 获取 | `hook_d` |
| **Plugin 守护进程** | `plugin_d/` | 插件生命周期管理与隔离 | `plugin_d` |
| **公共库** | `common/` | 共享工具库与兼容层（18+ 组件）—— `svc_common` | `svc_common` |

### 架构总览

```
+-------------------------------------------------------------------+
|                        外部客户端/Agent                              |
+-------------------------------------------------------------------+
|  gateway_d (API 网关)                                              |
|  HTTP/WS/Stdio/MCP/A2A/OpenAI API → JSON-RPC 2.0 → 服务路由       |
+---+---------------+---------------+---------------+---------------+
    |               |               |               |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  |  llm_d    |  |  tool_d   |  |  sched_d  |  | market_d  |      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  |  monit_d  |  | channel_d |  |  info_d   |  | notify_d  |      |
|  +-----------+  +-----------+  +-----------+  +-----------+      |
|  +-----------+  +-----------+  +--------------------------------+ |
|  | observe_d |  |  hook_d   |  |  plugin_d                      | |
|  +-----------+  +-----------+  +--------------------------------+ |
|  +--------------------------------------------------------------+ |
|  |  common (svc_common — 18+ 组件，传递链接 Cupolas 安全穹顶)        |
|  +--------------------------------------------------------------+ |
+-------------------------------------------------------------------+
|                       Airymax 内核层 (atoms)                       |
+-------------------------------------------------------------------+
```

---

## 3. 上游 / 下游依赖关系

### 上游（Daemons 依赖）

| 依赖 | 来源 | 用途 |
|------|------|------|
| **atoms** | `agentrt/atoms/` | CoreLoopThree（认知/执行/记忆循环）、Syscall 入口表面、TaskFlow 编排、Memory 原语——`hook_d` 直接链接 `agentrt_coreloopthree`；每个守护进程通过 `atoms/syscall` 派发业务逻辑 |
| **commons** | `agentrt/commons/` | 日志、config_unified、网络、令牌、成本、可观测性、认知、策略——通过 `svc_common` 传递链接 |
| **cupolas** | `agentrt/cupolas/` | `svc_common` 以 `PUBLIC` 形式链接 Cupolas（`daemon_cupolas_bootstrap.c`），每个守护进程自动继承 Cupolas 安全——请求鉴权、输入净化、审计、沙箱 |
| **protocols** | `agentrt/protocols/` | IPC 服务总线使用的 JSON-RPC 2.0 / AgentsIPC 信封；网关边界使用 A2A / MCP 适配器 |
| **heapstore** | `agentrt/heapstore/` | 守护进程状态持久化——`market_d`/`tool_d`/`llm_d` 有专用数据目录；注册表追踪 Agent/Skill/Session；Token 引擎预算 LLM 用量 |
| **gateway** | `agentrt/gateway/` | `gateway_d` 封装网关库并以系统服务形式暴露 |
| cJSON / libcurl / libyaml / OpenSSL | 外部 | JSON 解析、HTTP 客户端、YAML 配置、TLS——由伞仓 CMake 自动检测 |

### 下游（消费 Daemons）

| 消费者 | 用途 |
|--------|------|
| **SDK / Agent 应用** | SDK 内置守护进程客户端库；Agent 应用通过网关的 JSON-RPC 2.0 表面调用运行时，消费守护进程服务（LLM、工具、调度、市场等） |
| OpenLab 应用 | OpenLab 模块通过 JSON-RPC 2.0 API 编排守护进程 |

### 内部依赖图

```
svc_common  ←  gateway_d  ←  外部客户端
          ←  llm_d        ←  gateway_d
          ←  tool_d       ←  gateway_d, llm_d
          ←  sched_d      ←  gateway_d
          ←  market_d     ←  gateway_d
          ←  monit_d      ←  所有守护进程（指标上报）
          ←  channel_d    ←  gateway_d
          ←  info_d       ←  gateway_d
          ←  notify_d     ←  monit_d（告警通知）
          ←  observe_d    ←  monit_d（可观测性）
          ←  hook_d       ←  sched_d, tool_d（Hook 注入）
          ←  plugin_d     ←  market_d, tool_d（插件生命周期）
```

---

## 4. 通信与生命周期

### IPC 服务总线

所有守护进程通过统一的 `ipc_service_bus` 通信，支持多协议消息传递：

| 通信方式 | 适用场景 | 延迟 | 协议 |
|----------|----------|------|------|
| Unix Socket | 同机守护进程 | < 100 μs | JSON-RPC 2.0 |
| TCP | 跨机守护进程 | < 1 ms | JSON-RPC 2.0 |
| 共享内存 | 高性能数据交换 | < 10 μs | 自定义 |

### 守护进程生命周期

```
INIT → CONFIG_LOAD → SERVICE_REGISTER → IDLE → BUSY → SHUTDOWN
 初始化   加载配置    注册到服务发现     等待    处理    优雅关闭
```

服务状态枚举（`agentrt_svc_state_t`）：`NONE / CREATED / INITIALIZING /
READY / RUNNING / PAUSED / STOPPING / STOPPED / ZOMBIE / ERROR`。

---

## 5. 构建说明

### 前置依赖

- CMake ≥ 3.16
- C11 编译器（GCC / Clang / MSVC）
- cJSON 库
- GTest（可选，用于单元测试）
- lcov / genhtml（可选，用于覆盖率报告）

### 构建命令

```bash
# 标准构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 启用测试
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build

# 启用覆盖率
cmake -B build -DBUILD_COVERAGE=ON
cmake --build build
cmake --build build --target coverage

# 跨平台构建
cmake -B build -DBUILD_ALL_PLATFORMS=ON
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTS` | `ON` | 构建单元测试 |
| `BUILD_COVERAGE` | `OFF` | 启用代码覆盖率 |
| `BUILD_ALL_PLATFORMS` | `OFF` | 跨平台编译 |

### 构建产物

- 12 个守护进程可执行文件：`gateway_d`、`llm_d`、`tool_d`、`sched_d`、
  `market_d`、`monit_d`、`channel_d`、`info_d`、`notify_d`、`observe_d`、
  `hook_d`、`plugin_d`——输出到 `${CMAKE_BINARY_DIR}/bin/`
- `svc_common` —— 每个守护进程消费的共享静态库
- 公共头文件安装到 `include/agentrt/`

### 安装

```bash
cmake --install build --prefix /opt/airymax
```

### 启动方式

```bash
# 启动单个守护进程
./build/bin/gateway_d --config gateway_config.json

# 使用管理器启动所有守护进程
./daemon_manager --start-all

# 查看守护进程状态
./daemon_manager --status
```

### CI/CD 脚本

| 脚本 | 用途 |
|------|------|
| `scripts/ci.sh` | CI 流水线构建脚本 |
| `scripts/local-ci.sh` | 本地 CI 模拟脚本 |
| `scripts/static-analysis.sh` | 静态代码分析 |
| `scripts/verify-coverage.sh` | 覆盖率验证 |

---

## 6. 许可证

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.

本模块采用双许可证，您可以选择以下任一许可证遵守：

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt))，或
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

完整许可证文本见 [LICENSE](LICENSE) 文件，版权声明见 [NOTICE](NOTICE)。
默认适用 AGPL-3.0-or-later 条款；Apache-2.0 备选用于 AGPL 无法覆盖的
下游集成场景（如闭源或专有分发）。
