# common — 守护进程公共库

> **模块路径**: `agentrt/daemons/common/` · **CMake 目标**: `svc_common`（静态库）

[![version](https://img.shields.io/badge/version-0.1.16-blue)](https://atomgit.com/openairymax/daemons)
[![license](https://img.shields.io/badge/license-AGPL--3.0--or--later%20OR%20Apache--2.0-green)](../LICENSE)

## 这是什么

`common` 是 AgentRT 全部 15 个守护进程共享的**静态库**，不是可执行程序，也不监听任何
socket。它把守护进程样板收敛成一套可复用设施：服务生命周期与注册、JSON-RPC 方法分发、
跨进程服务发现与 RPC 客户端、认证与授权、事件驱动主循环、并行执行引擎、配置管理与统一日志。

所有 daemon 的 `main.c` 都经由本模块的 `daemon_main.h` 获取启动骨架，因此它同时是
`daemons` 层与下层 `commons` / `atoms` 之间的枢纽。

## 能力

- **启动骨架**：`daemon_main.h` 提供 `DAEMON_DECLARE_COMMON(<name>_d, <ns>, UNIX, WIN,
  PORT, MAX_BUFFER)` 宏，统一生成 Unix socket（Windows 为 TCP 回环）端点、信号处理、
  日志初始化与用法输出；`daemon_parse_args()` 解析 `--manager <path>`、`--tcp` 与
  `--help` 三个选项。
- **事件驱动主循环**：`daemon_event_driver` 提供连接池、任务队列与并发客户端处理，
  各 daemon 主循环基于它跑通（含 JSON-RPC 分发）。
- **JSON-RPC 分发**：`method_dispatcher_*` 注册表式方法路由，`jsonrpc_helpers_*` 负责
  请求解析与成功/错误响应构建。
- **跨进程通信**：`daemon_rpc_client`（Unix socket / Windows TCP 回环上的精简 JSON-RPC
  客户端，`gateway_d` 转发与各 daemon 互调都走它）。
- **服务发现**：`service_discovery*` 提供注册、发现、健康、选择与负载均衡，后端可切
  共享内存或文件；`daemon_bootstrap_sd` 是一键引导封装。
- **安全**：`svc_auth*`（JWT / API Key / 限流）、`daemon_security*`（ACL 授权、输入消毒、
  包签名校验、凭据与审计），并与 `cupolas` 安全穹顶集成。
- **容错与可观测**：`api_recovery`（重试/降级/熔断策略）、`alert_manager`、
  `unified_metrics`、`log_sanitizer`。
- **运行时数据引导**：`daemon_heapstore_bootstrap`（运行时数据存储）；安全穹顶引导
  归 cupolas_d 策略单元 `airy_security_dome`（0.1.19 §254b 机制/策略分离）。
- **ops 表注入**：`daemon_ipc_ops_bootstrap` 把上述能力的函数表注入 `atoms` 侧抽象接口，
  使 `atoms` 无需反向链接 `daemons` 即可调用 IPC/RPC/服务发现。

## 架构

```
各 daemon（gateway_d / llm_d / tool_d / sched_d / ...）
        │ 链接
        ▼
   svc_common（本模块，静态库）
        ├─ PRIVATE → airy_ipc_ops / airy_syscall_ops（ops 表存储小库，向 atoms 注入 IPC/RPC/SD 能力）
        └─ PUBLIC  → airy_common（commons 统一基础库）
                     ├─ cupolas（可选，存在 target 时 PUBLIC 传播给所有 daemon）
                     ├─ airy_heapstore（可选，BUILD_HEAPSTORE）
                     └─ OpenSSL / cJSON / YAML / CURL / Threads（可选）
```

### 兼容再导出头

`include/` 下共 14 个头文件。除 `daemon_defaults.h` 外（再导出兼容头，本体只有一行
`#include "airy_defaults.h"`，指向 `commons` 仓内的权威版本），其余 13 个都是本模块
自有的框架/桥接头（`daemon_main.h`、`daemon_event_driver.h`、`daemon_security.h`、
`svc_auth.h`、`svc_config.h`、`daemon_dep.h` 等），
仅在源码树内消费。

> 早前的再导出兼容头家族（`svc_common.h`、`method_dispatcher.h`、`param_validator.h`、
> `thread_pool.h`、`airy_event_loop.h`、`unified_metrics.h`、`api_recovery.h`、
> `log_sanitizer.h`、`jsonrpc_helpers.h`、`alert_manager.h`、`circuit_breaker.h`、
> `service_discovery.h`、`platform.h`、`compat.h`、`error.h` 等）已在 0.1.17~0.1.19
> 各施工线随权威实现归位 `commons` 时逐批拆除：`svc_common` 的 PUBLIC include 路径以
> commons 权威目录优先，裸引直接解析权威版本，兼容头已无必要。

为此，`svc_common` 的 PUBLIC include 路径把 `commons` 各权威目录声明在
`daemons/common/include` **之前**，保证下层代码优先解析到权威版本，不会出现跨层反向依赖。

## 构成

`src/` 按功能域组织，共 25 个 C 源文件：

| 域 | 数量 | 源文件 |
|----|------|--------|
| `src/svc/` | 4 | `svc_common.c`（服务生命周期核心）、`svc_common_registry.c`（进程内注册表）、`svc_common_ops.c`（状态查询/异步请求）、`svc_model_defaults.c`（`model.yaml` 全局默认模型提取，`llm_d` / `gateway_d` 共用） |
| `src/auth/` | 6 | `svc_auth.c`（认证中间件聚合）、`svc_auth_jwt.c` / `svc_auth_jwt_crypto.c` / `svc_auth_jwt_verify.c`（JWT 生命周期、HMAC/Base64 原语、签名校验）、`svc_auth_apikey.c`、`svc_auth_ratelimit.c` |
| `src/security/` | 4 | `daemon_security.c`（初始化/消毒）、`_acl.c`（ACL 授权）、`_signature.c`（包签名验证）、`_vault.c`（凭据与审计） |
| `src/daemon/` | 5 | `daemon_event_driver.c`（事件驱动主循环）、`daemon_rpc_client.c`、`daemon_dep.c`（硬依赖探测与降级上报）、`daemon_heapstore_bootstrap.c`、`daemon_ipc_ops_bootstrap.c` |

> 跨进程服务发现全家族（原 `src/discovery/` 7 文件）已迁 `commons/utils/sd/`
> （g14 补完 P0.17 阶段 4 半程迁移），符号经 `airy_common` PUBLIC 链接提供；
> 服务发现单元测试亦随迁 `commons/tests/unit/`。

## 接口

本模块**不暴露 JSON-RPC 端点**，它向各 daemon 提供 C API：

- `method_dispatcher_create / destroy / register / dispatch`：daemon `main.c` 用
  `method_dispatcher_register(disp, "<method>", handler, NULL)` 注册自己的方法。
- `jsonrpc_build_success / jsonrpc_build_error / jsonrpc_parse_request /
  jsonrpc_get_string_param / jsonrpc_get_int_param`：请求解析与响应封装。
- `daemon_rpc_call` / `daemon_rpc_call_cancelable` / `daemon_rpc_call_stream`：
  daemon 之间的 JSON-RPC 客户端调用（`gateway_d` 转发、`sched_d` 派发、流式响应均使用）。
- `daemon_ipc_ops_init`：向 `atoms` 注入 IPC/RPC/服务发现能力函数表。LLM 与工具域的
  同类引导分别由 `llm_d`、`tool_d` 自己提供。
- 各 daemon 公共方法（`health_check` / `get_stats` / `shutdown` 等）的实现样板也来自这里，
  具体方法名以各 daemon README 的接口表为准。

## 配置

本模块无独立运行时配置文件。路径由 `commons` 的平台路径系统决定：

- 编译期默认：Linux/macOS `/etc/agentrt`（配置）、`/var/log/agentrt`（日志）、
  `/tmp/agentrt`（运行时）；Windows `C:\ProgramData\agentrt\*`。
- 运行期：`airy_paths_init()` 按 `$AIRY_HOME`（默认 `$HOME/.airymaxrt`）解析并创建目录，
  同时导出 `AIRY_CONFIG_DIR` / `AIRY_LOG_DIR` / `AIRY_RUNTIME_DIR` 等环境变量，
  因此实际生效路径是 `$AIRY_HOME/config`、`$AIRY_HOME/data/agentrt/logs`、
  `$AIRY_HOME/run`。

## 用法

`common` 不单独运行，随任一 daemon 一起构建：

```bash
# 在 agentrt/daemons 目录下
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target svc_common
```

Windows 源码构建时守护进程默认关闭，需显式打开：

```bash
cmake -S . -B build -DBUILD_DAEMON=ON -DBUILD_CLI=ON
```

## 测试

`tests/` 下的目标以 `svc_test_` 前缀注册进 CTest，共 20 个用例；`BUILD_TESTS` 为 `ON`
且非 Windows 时才加入构建。

- 基础：`svc_test_error` / `svc_test_platform` / `svc_test_logger` / `svc_test_config` /
  `svc_test_safe_string_utils`
- 服务框架与分发：`svc_test_svc_auth` / `svc_test_jsonrpc_helpers` / `svc_test_svc_stop` /
  `svc_test_daemon_common`（按功能域拆分为 6 个测试文件）
- 安全：`svc_test_daemon_security` / `svc_test_log_sanitizer`
- 容错与并发：`svc_test_strategies_recovery` / `svc_test_api_recovery`
  （含 pool / cred / health / fallback / config / misc 六个域文件）/
  `svc_test_thread_pool` / `svc_test_airy_event_loop` / `svc_test_checkpoint`
- 引导与其他：`svc_test_svc_model_defaults`。

```bash
ctest --test-dir ../daemons-build/common -R "^svc_test_" -V
```

## 依赖

| 依赖 | 用途 |
|------|------|
| [commons](https://atomgit.com/openairymax/commons) | `airy_common` 统一基础库：错误码、日志、内存、字符串、同步、缓存、可观测性、平台路径 |
| [atoms](https://atomgit.com/openairymax/atoms) | `airy_core`（corekern IPC 通道/事务）、`airy_ipc_ops` 与 `airy_syscall_ops`（ops 表存储小库）、`coreloopthree` 头文件路径 |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 可选；存在时 PUBLIC 链接，所有 daemon 自动获得安全穹顶能力 |
| [heapstore](https://atomgit.com/openairymax/heapstore) | 可选；`BUILD_HEAPSTORE=ON` 时 PUBLIC 链接 `airy_heapstore` |
| 外部 | `Threads::Threads`；可选 `OpenSSL`、`cJSON`、`libyaml`、`CURL`；Windows 另链 `ws2_32`、`bcrypt`、`advapi32` |

安装面只包含 `svc_common` 归档（`lib/`），`include/` 下的兼容层与桥接头不安装——
对外 API 头由各下层库自行安装，避免出现第二个不一致的公共 API 面。

## 关系

- 被 [daemons](../README_zh.md) 下全部 15 个守护进程链接，是本层唯一的公共依赖入口。
- 向 [atoms](https://atomgit.com/openairymax/atoms) 注入 IPC/RPC/服务发现 ops 表，
  使下层保持对用户态运行时的无依赖。
- 与 [gateway_d](../gateway_d/README.md) 的关系最紧密：网关到各 daemon 的转发客户端、
  cap 白名单、鉴权与限流都建立在本模块之上。

## 许可

AGPL-3.0-or-later OR Apache-2.0，详见 [LICENSE](../LICENSE)。

Copyright (c) 2025-2026 SPHARX Ltd.
