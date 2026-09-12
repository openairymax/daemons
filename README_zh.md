# daemons — 运行时守护进程服务

> Airymax 智能体运行时的用户态服务层：15 个守护进程把 Airymax 内核变成一个真正在跑的
> 系统，外加共享库 `svc_common`。

**语言：** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)

- **仓库：** <https://atomgit.com/openairymax/daemons>
- **版本：** 0.1.15
- **许可证：** AGPL-3.0-or-later OR Apache-2.0

---

## 这是什么

**daemons** 是 Airymax 智能体运行时的服务层，包含 **15 个长驻守护进程**——`gateway_d`、
`llm_d`、`tool_d`、`sched_d`、`market_d`、`monit_d`、`channel_d`、`notify_d`、`hook_d`、
`mem_d`、`agent_d`、`a2a_d`、`think_d`、`cupolas_d`、`maths_d`——以及共享静态库
`svc_common`（位于 `common/`）。

每个守护进程都是独立的操作系统进程，各自只负责一个领域，对外暴露 JSON-RPC 2.0 接口，
并通过 IPC 服务总线与同伴通信。`gateway_d` 是唯一面向外部客户端的进程边界，其余守护进程
都留在内部。

```
外部客户端 ──HTTP / WS / SSE / MCP / A2A / OpenAI API──▶ gateway_d
                                                        │
                                              IPC 总线上的 JSON-RPC 2.0
                                                        ▼
                        llm_d  tool_d  sched_d  mem_d  agent_d …（14 个后端）
                                                        │
                                              atoms / syscall ──▶ 内核
```

## 能力

- **服务化** —— 独立进程、IPC 协作；每个守护进程可单独启动、扩缩、升级与替换。
- **职责单一** —— 每个守护进程只负责一个核心领域，耦合度低。
- **安全内生** —— `svc_common` 以 `PUBLIC` 形式链接 `cupolas`，每个守护进程无需自己编写
  安全代码，即自动继承请求鉴权、输入净化、审计与沙箱。
- **协议统一** —— 运行时内部一律 JSON-RPC 2.0；MCP / A2A / OpenAI-API 转换只发生在网关边界。
- **韧性** —— 熔断器、带主备切换的 API 恢复、健康检查、降级服务自动恢复。
- **可观测** —— 所有守护进程向 `monit_d` 上报指标、向 `notify_d` 上报事件，并按进程落盘
  日志，用 `airymaxrt logs <daemon>_d` 即可查看。
- **统一生命周期框架** —— 15 个进程共用一套 `airy_svc_t` 状态机与事件驱动主循环
  （`daemon_event_driver`）。

## 15 个守护进程

| # | 守护进程 | RPC 命名空间 | 职责 |
|---|----------|--------------|------|
| 1 | [gateway_d](gateway_d/README.md) | —（入口） | 唯一外部边界。把 HTTP / WebSocket / SSE / MCP / A2A / OpenAI API 翻译为 JSON-RPC 2.0，按命名空间转发到后端。不含业务逻辑。 |
| 2 | [llm_d](llm_d/README.md) | `llm.*` | LLM 推理：流式补全、token 计数、成本核算、响应缓存。 |
| 3 | [tool_d](tool_d/README.md) | `tool.*`、`plugin.*` | 工具与插件注册、发现、沙箱执行、参数校验、结果缓存。 |
| 4 | [sched_d](sched_d/README.md) | `sched.*` | 任务与 DAG 调度、路线图规划、轮询 / 加权 / 优先级 / ML 四种调度策略。 |
| 5 | [market_d](market_d/README.md) | `market.*` | Agent / Skill / Tool / Template 工件：检索、安装、版本管理、卸载。 |
| 6 | [monit_d](monit_d/README.md) | `monit.*` | 指标采集与查询、系统与硬件信息、健康检查、告警规则、Agent 死循环检测。 |
| 7 | [channel_d](channel_d/README.md) | `channel.*` | 数据面应用通道：通道创建 / 加入 / 收发与消息路由。 |
| 8 | [notify_d](notify_d/README.md) | `notify.*` | 事件扇出：基于 topic 的发布订阅，覆盖 WebSocket、SSE 与 socket。 |
| 9 | [hook_d](hook_d/README.md) | `hook.*` | Hook 与会话注册；Hook 引擎本体位于 `atoms/coreloopthree`。 |
| 10 | [mem_d](mem_d/README.md) | `mem.*` | 持久化记忆：写入 / 检索 / 读取 / 删除 / recent / evolve，TF-IDF + embedding 混合检索，JSONL 存储。 |
| 11 | [agent_d](agent_d/README.md) | `agent.*` | Agent 生命周期与执行循环：`run` / `run_stream` / `run_cancel`、spawn / invoke / terminate / cancel。 |
| 12 | [a2a_d](a2a_d/README.md) | `a2a.*` | Agent 间协议：Agent Card 注册与发现、任务状态机、消息投递。 |
| 13 | [think_d](think_d/README.md) | `think.*` | 认知服务：两段式交互、流程编排、语言前置、反思评审。 |
| 14 | [cupolas_d](cupolas_d/README.md) | `cupolas.*`、`policy.*` | 安全策略决策点：权限校验、输入净化、审计、凭据库、网络规则、策略加载 / 生效 / 回滚。 |
| 15 | [maths_d](maths_d/README.md) | `maths.*` | 数学外挂计算：纯 C 数值与统计求值，外加可选符号计算后端。 |

可执行文件名保留 `*_d` 后缀，与 CMake target 名一一对应（`gateway_d`、`llm_d`……）。
各目录内的 README 记录该进程的具体接口。

## 目录结构

```
daemons/
├── CMakeLists.txt      # 构建 15 个守护进程 + svc_common
├── common/             # svc_common 静态库（共享服务框架）
├── scripts/            # CI、本地验证、静态分析、覆盖率
├── gateway_d/ … maths_d/   # 每个守护进程一个目录
├── Dockerfile.ci       # CI 构建环境
├── LICENSE             # AGPL-3.0 + Apache-2.0 双许可证全文
└── NOTICE              # 版权与核心 IP 声明
```

每个守护进程目录结构一致：

```
<name>_d/
├── CMakeLists.txt      # target <name>_d
├── README.md           # 职责、RPC 接口、依赖、启动与排查方式
├── include/            # 公共头文件
├── src/                # 源码（main.c 注册 JSON-RPC 方法）
└── tests/              # 单元测试
```

### svc_common（`common/`）

`common/` 构建 `svc_common` 静态库，被每个守护进程以 `PRIVATE` 形式链接。它提供服务框架
（`airy_svc_t`、事件驱动、任务派发器，以及 IPC / systemd / Cupolas 的 bootstrap）、IPC
客户端与服务总线、JSON-RPC 方法派发器与参数校验、韧性组件（熔断器、API 恢复、输入校验、
日志净化）、指标与告警、配置，以及平台兼容层。详见 [`common/README.md`](common/README.md)。

## 用法

### 运行

运行时由 `airymaxrt` 启动器管理，守护进程集群的拉起与收摊都由它负责，通常无需手动启动
某个守护进程。

```bash
airymaxrt                 # 终端界面——拉起运行时及其服务
airymaxrt status          # 当前运行状态
airymaxrt doctor          # 组件健康检查
airymaxrt logs 100        # 最近 100 行运行日志
airymaxrt logs llm_d      # 指定守护进程的日志
airymaxrt monitor         # 持续观察
```

其余子命令为 `cli`、`profile`、`update`、`uninstall`、`reinstall`。
**没有 `airymaxrt start`** —— 不带参数运行启动器即拉起全部服务。

若要单独验证某个守护进程的接口，可直接启动它并向其 socket 发送 JSON-RPC：

```bash
<build-dir>/bin/maths_d                     # 监听在运行时目录
```

POSIX 上端点是运行时目录下的 Unix socket `<runtime-dir>/<name>.sock`，从运行时根目录解析，
手动启动的守护进程与由启动器拉起的处在同一条总线上。Windows 上守护进程监听本机 TCP 回环
`127.0.0.1:<port>`，各守护进程的默认端口见其各自的 README。

### 从源码构建

前置依赖：CMake ≥ 3.16、C11 编译器（GCC / Clang / MSVC）、cJSON。
可选：GTest（单元测试）、lcov + genhtml（覆盖率）、cppcheck（静态分析）。

```bash
cmake -S . -B ../daemons-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../daemons-build --parallel
```

构建目录需位于源码树之外。

CMake 选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_DAEMON` | POSIX `ON`，Windows `OFF` | 构建守护进程集群（由上层构建设置） |
| `BUILD_TESTS` | `ON`（Windows 上强制 `OFF`） | 构建单元测试并启用 CTest |
| `BUILD_COVERAGE` | `OFF` | 覆盖率插桩并添加 `coverage` 目标 |
| `BUILD_ALL_PLATFORMS` | `OFF` | 跨平台编译 |

以 `BUILD_DAEMON=OFF` 配置时，本模块会输出一条警告并跳过。在 Windows 上需要守护进程与
CLI 时，显式开启：

```bash
cmake -S . -B ../agentrt-build -DBUILD_DAEMON=ON -DBUILD_CLI=ON
```

官方 Windows 发布包本身已包含守护进程与 CLI；只有纯源码构建默认关闭它们。

产物与安装：

```bash
ctest --test-dir ../daemons-build --output-on-failure
cmake --install ../daemons-build --prefix /opt/airymax   # 可执行文件 → <prefix>/bin
```

- `${CMAKE_BINARY_DIR}/bin/` 下的 15 个守护进程可执行文件
- `svc_common` 静态库，由各守护进程私有链接
- 守护进程公共头文件安装到 `include/agentrt/`

### CI 脚本

| 脚本 | 用途 |
|------|------|
| [`scripts/`](scripts/README.md) | CI 入口：构建、测试、cppcheck、覆盖率 |
| `scripts/local-ci.sh` | 本地 CI 模拟 |
| `scripts/static-analysis.sh` | cppcheck 静态分析 |
| `scripts/verify-coverage.sh` | 覆盖率收集与阈值验证 |

## 接口

**服务生命周期** —— 所有守护进程共用一套状态机（`airy_svc_state_t`）：
`NONE → CREATED → INITIALIZING → READY → RUNNING → PAUSED → STOPPING → STOPPED`，
另有停止超时对应的 `ZOMBIE` 与故障对应的 `ERROR`。启动次序为
`初始化 → 加载配置 → 注册到服务发现 → 提供服务 → 优雅关闭`。

**能力标志** —— 守护进程通过 `airy_svc_config_t.capabilities` 声明自己支持的能力：
`AIRY_SVC_CAP_NONE / ASYNC / STREAMING / CANCELABLE / PAUSEABLE / THROTTLE / BATCH /
PRIORITY / TIMEOUT`。

**错误码** —— 使用 `commons` 中的标准 `AIRY_E*` 集合，并由 `daemon_errors.h` 叠加守护进程
层面的别名。

```c
#include "svc_common.h"
#include "ipc_service_bus.h"

int main(void)
{
    airy_svc_config_t cfg = {
        .name           = "my_daemon",
        .version        = "0.1.15",
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

## 关系

daemons 是组合层：它不定义内核原语，而是把原语组织成运行中的进程。

| 依赖 | daemons 使用它的什么 |
|------|---------------------|
| [commons](https://atomgit.com/openairymax/commons) | 日志、配置、网络、令牌、成本、可观测性、平台路径与权威 IPC 头文件——经 `svc_common` 传递链接 |
| [atoms](https://atomgit.com/openairymax/atoms) | 向下游派发的 Syscall 入口表面；`hook_d` 直接链接 CoreLoopThree 的 hook 库 |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 安全穹顶，由 `svc_common` 以 `PUBLIC` 链接；`cupolas_d` 将其作为服务暴露 |
| [protocols](https://atomgit.com/openairymax/protocols) | IPC 总线上的 JSON-RPC 2.0 / AgentsIPC 信封；网关边界的 A2A 与 MCP 适配器 |
| [heapstore](https://atomgit.com/openairymax/heapstore) | 守护进程状态、注册表与配额的持久化 |
| [gateway](https://atomgit.com/openairymax/gateway) | `gateway_d` 封装并作为系统服务暴露的网关库 |

| 消费者 | 使用内容 |
|--------|----------|
| SDK / Agent 应用 | 网关的 JSON-RPC 2.0 表面，经 SDK 内置的守护进程客户端库访问 |
| 命令行与终端界面 | 记忆读写、认知、状态查看与日志 |
| 生态工具与 Skills | 通过 SDK 访问守护进程服务 |

## 文档

设计文档、接口参考与应用开发指南位于
[Airymax 文档仓库](https://atomgit.com/openairymax/docs)的 `AirymaxRT/` 目录下。
入口见 `AirymaxRT/README.md`，API 参考见 `AirymaxRT/30-interfaces/`。

## 许可证

Copyright (c) 2025-2026 SPHARX Ltd.

本模块采用双许可证，可选择以下任一许可证遵守：

- **GNU Affero General Public License v3.0 or later**
  ([AGPL-3.0-or-later](https://www.gnu.org/licenses/agpl-3.0.txt))，或
- **Apache License, Version 2.0**
  ([Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0.txt))

SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`

完整许可证文本见 [LICENSE](LICENSE)，版权与核心 IP 声明见 [NOTICE](NOTICE)。
默认适用 AGPL-3.0-or-later 条款；Apache-2.0 备选用于 AGPL 无法覆盖的下游集成场景。
