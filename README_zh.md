# daemons — 运行时守护进程服务（18 个守护进程）

> Airymax 智能体运行时的用户态服务层：Airymax 内核之上的后端服务支撑。
> [agentrt](../) 管理仓下的叶子仓。

**语言:** [English](README.md) | 简体中文

[![Version](https://img.shields.io/badge/version-0.1.5-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **仓库：** `git@atomgit.com:openairymax/daemons.git`
- **分支：** `feature/official-hubs-01`
- **版本：** 0.1.5（与 agentrt 管理仓对齐）

---

## 概览

**daemons** 是 Airymax 智能体运行时的**用户态服务层**。它由 **18 个独立守护进程**——`gateway_d / llm_d / tool_d / sched_d / market_d / monit_d / channel_d / info_d / notify_d / observe_d / hook_d / plugin_d / mem_d / agent_d / a2a_d / think_d / cupolas_d / maths_d`——以及共享静态库 `svc_common`（位于 `common/`）共同组成。每个守护进程遵循**职责单一原则**：独立进程运行，通过统一 IPC 服务总线协作通信，共同构成位于 Airymax 内核之上的高可用、可扩展、可插拔微服务架构。

```
外部客户端 → gateway_d → (其他守护进程经 ipc_service_bus) → atoms/syscall → 内核服务
  (HTTP/WS/Stdio)  (守护进程)
```

设计目标：

- **服务化架构** —— 每个守护进程独立运行，通过 IPC 协作，支持独立部署与扩缩容。
- **职责单一** —— 每个守护进程只负责一个核心领域，降低耦合度。
- **可插拔** —— 守护进程可独立部署、升级和替换，不影响其他服务。
- **高可用** —— 支持主备切换、熔断器保护、故障转移和自动恢复。
- **安全内生** —— `svc_common` 以 `PUBLIC` 形式链接 `cupolas`（`daemon_cupolas_bootstrap.c`），每个守护进程自动继承 Cupolas 安全：请求鉴权、输入净化、审计、沙箱。
- **协议统一** —— 所有守护进程通过 JSON-RPC 2.0 通信；MCP / A2A / OpenAI-API 协议转换发生在网关边界。

在 Airymax 0.1.3 发行版中，`daemons` 是 [agentrt](../) 管理仓聚合的 7 个叶子仓之一，构成循环分层架构中的**服务层**（位于网关层 `gateway` 之上、生态层 `sdk`/`ecosystem` 之下）。它是 agentrt 内部最顶层的叶子仓——每个守护进程的业务逻辑通过 `atoms/syscall` 向下派发至内核。

## 模块分类

**— 类（服务 / 组合层）。**

daemons 是服务/组合模块：它不提供基础原语，而是将原语组合为运行中的进程。它依赖 `atoms`（CoreLoopThree / Syscall / TaskFlow / Memory 原语——`hook_d` 直接链接 `airy_coreloopthree`；每个守护进程通过 `atoms/syscall` 派发）、`commons`（日志、config_unified、网络、令牌、成本、可观测性、认知、策略——通过 `svc_common` 传递链接）、`cupolas`（安全穹顶，由 `svc_common` 以 `PUBLIC` 形式链接）、`protocols`（IPC 服务总线使用的 JSON-RPC 2.0 / AgentsIPC 信封；网关边界使用 A2A / MCP 适配器）、`heapstore`（守护进程状态持久化）、`gateway`（`gateway_d` 守护进程封装网关库）。它的主要消费者是 SDK / Agent 应用（通过网关的 JSON-RPC 2.0 表面）和 OpenLab 模块。

## 目录结构

```
daemons/
├── CMakeLists.txt                 # 顶层构建文件；管理全部 18 个守护进程 + svc_common
├── Dockerfile.ci                  # CI 环境 Docker 镜像
├── README.md                      # 英文版
├── README_zh.md                   # 本文件（中文）
├── LICENSE                        # 双许可证文本（AGPL-3.0 + Apache-2.0）
├── NOTICE                         # 版权声明
├── common/                        # 共享服务库（svc_common）
│   ├── CMakeLists.txt             # svc_common 静态库 target
│   ├── README.md                  # svc_common 文档
│   ├── include/                   # 共享头文件
│   ├── src/                       # 源文件（工具组件）
│   └── tests/                     # svc_common 单元测试
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
├── mem_d/                         # 记忆守护进程（mem.* 命名空间，JSONL 持久化）
├── agent_d/                       # Agent 执行守护进程（agent.* 命名空间）
├── a2a_d/                         # Agent 间通信（A2A）守护进程（a2a.* 命名空间）
├── think_d/                       # 双思考 / GRAD 认知守护进程（think.* 命名空间）
├── cupolas_d/                     # Cupolas 安全穹顶守护进程（cupolas.* 命名空间）
├── maths_d/                       # 数学外挂计算守护进程（maths.* 命名空间）
└── scripts/                       # 构建 / CI / 分析脚本
    ├── ci.sh                      # CI 流水线构建脚本
    ├── local-ci.sh                # 本地 CI 模拟脚本
    ├── static-analysis.sh         # 静态代码分析
    └── verify-coverage.sh         # 覆盖率验证
```

### svc_common 共享库（`common/`）

`common/` 子目录编译为 `svc_common` 静态库，被每个守护进程以 `PRIVATE` 形式链接。它在单一 ABI 表面下聚合 30+ 工具组件：

| 分类 | 组件 |
|------|------|
| **服务框架** | `svc_common.c`、`svc_auth.c`、`svc_cache.h`、`svc_config.h`、`svc_logger.h`、`service_discovery.c`、`service_discovery_helper.c`、`daemon_bootstrap_ipc.c`、`daemon_bootstrap_sd.c`、`daemon_cupolas_bootstrap.c`、`daemon_startup.h`、`daemon_event_driver.c`、`daemon_task_dispatcher.c` |
| **韧性与安全** | `circuit_breaker.c`、`api_recovery.c`、`daemon_degradation.c`、`daemon_security.c`、`daemon_oom.c`、`input_validator.c`、`log_sanitizer.c`、`ipc_backpressure.c` |
| **IPC 与消息** | `ipc_service_bus.c`、`ipc_client.c`、`ipc_bus_helper.c`、`daemon_bootstrap_ipc.h`、`method_dispatcher.c`、`jsonrpc_helpers.c`、`param_validator.c` |
| **事件与并发** | `airy_event_loop.c`、`thread_pool.c`、`refcount.c` |
| **指标与告警** | `unified_metrics.c`、`alert_manager.c` |
| **配置** | `config_manager.c`、`daemon_defaults.h`、`daemon_errors.h`、`daemon_platform_ext.h` |
| **内存** | `arena.c`、`tcache.c`（守护进程本地分配器） |
| **平台** | `platform_compat.c`、`compat.h`、`platform.h` |

> **P0.17 阶段 3 / IRON-6：** `svc_common.h` 与 `ipc_service_bus.h` 的权威定义已迁移至 `commons/utils/ipc/include/`。`common/include/` 下的 daemons 侧头文件保留为**重导出兼容头**，使内部源文件无需立即修改 `#include` 路径，消除 atoms→daemons 编译期反向依赖。

## 核心组件

### 18 个守护进程

| # | 守护进程 | 目录 | 职责 | CMake Target |
|---|----------|------|------|--------------|
| 1 | **API 网关** | `gateway_d/` | 外部请求接入；协议转换（HTTP / WS / MCP / A2A / OpenAI API）与路由——封装 `gateway` 库 | `gateway_d` |
| 2 | **LLM 服务** | `llm_d/` | 大语言模型调用、Token 计数、成本追踪、响应缓存 | `llm_d` |
| 3 | **工具执行** | `tool_d/` | 工具注册 / 发现、沙箱执行、参数校验、结果缓存 | `tool_d` |
| 4 | **任务调度** | `sched_d/` | 任务分发，4 种调度策略（轮询 / 加权 / 优先级 / ML） | `sched_d` |
| 5 | **应用市场** | `market_d/` | Agent / Skill / Tool / Template 资源管理、安装、版本控制 | `market_d` |
| 6 | **监控告警** | `monit_d/` | 指标采集、健康检查、告警管理、Agent 死循环检测 | `monit_d` |
| 7 | **通道服务** | `channel_d/` | 通信通道管理与消息路由 | `channel_d` |
| 8 | **信息服务** | `info_d/` | 系统信息查询与状态报告 | `info_d` |
| 9 | **通知服务** | `notify_d/` | 多渠道通知推送（邮件 / Slack / Discord） | `notify_d` |
| 10 | **观测服务** | `observe_d/` | OpenTelemetry 可观测性数据采集 | `observe_d` |
| 11 | **Hook 守护进程** | `hook_d/` | 薄守护进程壳；Hook 系统核心位于 `atoms/coreloopthree/src/hook/`，通过链接 `airy_coreloopthree` 获取 | `hook_d` |
| 12 | **Plugin 守护进程** | `plugin_d/` | 插件生命周期管理与隔离 | `plugin_d` |
| 13 | **记忆守护进程** | `mem_d/` | 运行时记忆管理（`mem.*` 命名空间）：长时记忆的写入 / 检索 / 读取 / 删除，JSONL 持久化 | `mem_d` |
| 14 | **Agent 执行** | `agent_d/` | Agent 编排（`agent.*` 命名空间）：Agent 子进程 spawn / invoke / 健康检查 | `agent_d` |
| 15 | **A2A 协议** | `a2a_d/` | Agent 间通信（`a2a.*` 命名空间）：A2A 协议消息交换 | `a2a_d` |
| 16 | **双思考认知** | `think_d/` | 双思考 / GRAD 批判循环引擎（`think.*` 命名空间）：think.process / think.orchestrate / think.get_stats | `think_d` |
| 17 | **Cupolas 安全穹顶** | `cupolas_d/` | 独立安全穹顶（`cupolas.*` 命名空间）：权限引擎、输入净化、审计日志、隔离工位 | `cupolas_d` |
| 18 | **数学外挂计算** | `maths_d/` | 数学表达式求值 / 统计 / 表达式识别（`maths.*` 命名空间）：纯 C 递归下降求值器，零外部依赖，仅沙箱数值求值 | `maths_d` |

> **二进制命名规范：** 每个守护进程可执行文件保留 `*_d` 后缀（`gateway_d / llm_d / tool_d / sched_d / market_d / monit_d / channel_d / info_d / notify_d / observe_d / hook_d / plugin_d / mem_d / agent_d / a2a_d / think_d / cupolas_d / maths_d`）。根据 2026-07-05 改名决策，模块名从 `daemon` 统一为 `daemons`（目录、CMake target `airy_daemons`、仓库 `daemons.git`），但 18 个进程二进制名被刻意保留。

> **0.1.3 阶段 3 重构：** `mem_d / agent_d / a2a_d / think_d / cupolas_d` 从 gateway 进程拆分为独立 daemon（执行体集中化）。`gateway_d` 现在经 syscall 层（`airy_sys_svc_call`）转发这些命名空间，保持网关为纯协议边界。

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│                  外部客户端 / Agent 应用                       │
├──────────────────────────────────────────────────────────────┤
│   SDK (sdk-python / sdk-go / sdk-rust / sdk-typescript ...)   │
├──────────────────────────────────────────────────────────────┤
│   ★ daemons (服务层 — 18 个守护进程 + svc_common) ★          │
│                                                               │
│   gateway_d ─→ HTTP / WS / Stdio / MCP / A2A / OpenAI API     │
│              ↓                                                │
│   ┌────────┬────────┬────────┬────────┬────────┬─────────┐    │
│   │ llm_d  │tool_d  │sched_d │market_d│monit_d │channel_d│   │
│   ├────────┼────────┼────────┼────────┼────────┼─────────┤    │
│   │ info_d │notify_d│observe_d│hook_d │plugin_d│         │    │
│   ├────────┼────────┼────────┼────────┼────────┼─────────┤    │
│   │ mem_d  │agent_d │a2a_d   │think_d│cupolas_d│        │    │
│   └────────┴────────┴────────┴────────┴────────┴─────────┘    │
│              ↑ ipc_service_bus (JSON-RPC 2.0)                 │
│   ┌─────────────────────────────────────────────────────────┐ │
│   │ common (svc_common — 组件，PUBLIC 链接                  │ │
│   │ Cupolas → 每个守护进程继承安全)                         │ │
│   └─────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│   gateway / protocols / heapstore / cupolas                   │
├──────────────────────────────────────────────────────────────┤
│   atoms / commons / OS                                        │
└──────────────────────────────────────────────────────────────┘
```

**内部依赖图（svc_common ← 守护进程）：**

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
          ←  mem_d        ←  gateway_d, CLI/TUI（记忆读写）
          ←  agent_d      ←  gateway_d（Agent spawn/invoke）
          ←  a2a_d        ←  gateway_d（A2A 消息交换）
          ←  think_d      ←  gateway_d, CLI（双思考 / GRAD）
          ←  cupolas_d    ←  gateway_d, 所有守护进程（安全策略）
```

**设计原则：** 服务化（独立进程，IPC 协作）；每个守护进程职责单一；可插拔（独立部署 / 升级 / 替换）；高可用（主备、熔断器、故障转移）；安全内生（Cupolas 经 svc_common 传递链接）；协议统一（IPC 服务总线上的 JSON-RPC 2.0）。

## 上游依赖

> `svc_common` 是集成点，链接基础模块（`commons`、`cupolas`）并暴露给每个守护进程。daemons 还依赖 `atoms`、`protocols`、`heapstore`、`gateway`。

| 依赖 | 来源 | 用途 |
|------|------|------|
| **atoms** | `agentrt/atoms/` | CoreLoopThree（认知 / 执行 / 记忆循环）、Syscall 入口表面、TaskFlow 编排、Memory 原语——`hook_d` 直接链接 `airy_coreloopthree`；每个守护进程通过 `atoms/syscall` 派发业务逻辑 |
| **commons** | `agentrt/commons/` | 日志、config_unified、网络、令牌、成本、可观测性、认知、策略——通过 `svc_common` 传递链接。`svc_common.h` / `ipc_service_bus.h` 的权威定义按 IRON-6 现位于此（`commons/utils/ipc/include/`） |
| **cupolas** | `agentrt/cupolas/` | `svc_common` 以 `PUBLIC` 形式链接 Cupolas（`daemon_cupolas_bootstrap.c`），每个守护进程自动继承 Cupolas 安全——请求鉴权、输入净化、审计、沙箱 |
| **protocols** | `agentrt/protocols/` | IPC 服务总线使用的 JSON-RPC 2.0 / AgentsIPC 信封；网关边界使用 A2A / MCP 适配器 |
| **heapstore** | `agentrt/heapstore/` | 守护进程状态持久化——`market_d` / `tool_d` / `llm_d` 有专用数据目录；注册表追踪 Agent / Skill / Session；Token 引擎预算 LLM 用量 |
| **gateway** | `agentrt/gateway/` | `gateway_d` 封装网关库并以系统服务形式暴露 |
| cJSON / libcurl / libyaml / OpenSSL | 外部 | JSON 解析、HTTP 客户端、YAML 配置、TLS——由伞仓 CMake 自动检测（BAN-12） |

## 下游消费者

| 消费者 | 使用内容 |
|--------|----------|
| **SDK / Agent 应用** | SDK 内置守护进程客户端库；Agent 应用通过网关的 JSON-RPC 2.0 表面调用运行时，消费守护进程服务（LLM、工具、调度、市场等） |
| OpenLab 应用 | OpenLab 模块通过 JSON-RPC 2.0 API 编排守护进程 |
| 生态 ToolKit / Skills | Skills 与生态工具通过 SDK 访问守护进程服务 |

## 构建

### 前置依赖

- CMake ≥ 3.16
- C11 编译器（GCC / Clang / MSVC）
- cJSON 库
- GTest（可选，用于单元测试）
- lcov / genhtml（可选，用于覆盖率报告）

### 构建命令

```bash
# 标准构建
cmake -S . -B /tmp/daemons-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/daemons-build --parallel $(nproc)

# 启用测试
cmake -S . -B /tmp/daemons-build -DBUILD_TESTS=ON
cmake --build /tmp/daemons-build --parallel $(nproc)
ctest --test-dir /tmp/daemons-build --output-on-failure

# 启用覆盖率
cmake -S . -B /tmp/daemons-build -DBUILD_COVERAGE=ON
cmake --build /tmp/daemons-build --parallel $(nproc)
cmake --build /tmp/daemons-build --target coverage

# 跨平台构建
cmake -S . -B /tmp/daemons-build -DBUILD_ALL_PLATFORMS=ON
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTS` | `ON` | 构建单元测试 |
| `BUILD_COVERAGE` | `OFF` | 启用代码覆盖率 |
| `BUILD_ALL_PLATFORMS` | `OFF` | 跨平台编译 |

### 构建产物

- 18 个守护进程可执行文件：`gateway_d`、`llm_d`、`tool_d`、`sched_d`、`market_d`、`monit_d`、`channel_d`、`info_d`、`notify_d`、`observe_d`、`hook_d`、`plugin_d`、`mem_d`、`agent_d`、`a2a_d`、`think_d`、`cupolas_d`、`maths_d`——输出到 `${CMAKE_BINARY_DIR}/bin/`
- `svc_common` —— 每个守护进程消费（PRIVATE 链接）的共享静态库
- 公共头文件安装到 `include/agentrt/`

### 安装

```bash
cmake --install /tmp/daemons-build --prefix /opt/airymax
```

### 启动方式

```bash
# 启动单个守护进程
/tmp/daemons-build/bin/gateway_d --config gateway_config.json

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

## API

### 服务生命周期

服务状态枚举（`airy_svc_state_t`，定义于 `commons/utils/ipc/include/svc_common.h`）：

| 状态 | 说明 |
|------|------|
| `AIRY_SVC_STATE_NONE` | 未初始化 |
| `AIRY_SVC_STATE_CREATED` | 已创建 |
| `AIRY_SVC_STATE_INITIALIZING` | 初始化中 |
| `AIRY_SVC_STATE_READY` | 就绪 |
| `AIRY_SVC_STATE_RUNNING` | 运行中 |
| `AIRY_SVC_STATE_PAUSED` | 已暂停 |
| `AIRY_SVC_STATE_STOPPING` | 停止中 |
| `AIRY_SVC_STATE_STOPPED` | 已停止 |
| `AIRY_SVC_STATE_ZOMBIE` | 僵尸状态（停止超时 / 部分清理） |
| `AIRY_SVC_STATE_ERROR` | 错误状态 |

生命周期推进：

```
INIT → CONFIG_LOAD → SERVICE_REGISTER → IDLE → BUSY → SHUTDOWN
 初始化   加载配置    注册到服务发现     等待    处理    优雅关闭
```

### 服务能力标志（`airy_svc_capability_t`）

`AIRY_SVC_CAP_NONE / ASYNC / STREAMING / CANCELABLE / PAUSEABLE / THROTTLE / BATCH / PRIORITY / TIMEOUT`——每个守护进程通过 `airy_svc_config_t.capabilities` 位掩码声明其能力。

### IPC 服务总线

| 通信方式 | 适用场景 | 延迟 | 协议 |
|----------|----------|------|------|
| Unix Socket | 同机守护进程 | < 100 μs | JSON-RPC 2.0 |
| TCP | 跨机守护进程 | < 1 ms | JSON-RPC 2.0 |
| 共享内存 | 高性能数据交换 | < 10 μs | 自定义 |

### 错误码

守护进程扩展错误码通过 `daemon_errors.h` 暴露（经 `common/include/svc_common.h` 重导出）：`DAEMON_EINIT / ESTATE / EHEALTH` 等兼容别名，叠加在 `commons/include/airy_types.h` 中定义的标准 `AIRY_E*` 错误码集合之上。

### 使用示例

```c
#include "svc_common.h"
#include "ipc_service_bus.h"

int main(void) {
    /* 守护进程向 IPC 服务总线注册，声明能力。 */
    airy_svc_config_t cfg = {
        .name           = "my_daemon",
        .version        = "0.1.1",
        .capabilities   = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_CANCELABLE,
        .max_concurrent = 64,
        .timeout_ms     = 5000,
        .auto_start     = true,
        .enable_metrics = true,
    };
    /* svc_auth 自动继承 Cupolas 请求鉴权。 */
    return 0;
}
```

## 许可证

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
