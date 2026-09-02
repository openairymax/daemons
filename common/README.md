# Common — AgentRT 守护进程公共库

> **模块路径**: `agentrt/daemons/common/`

## 定位

`common` 是所有 AgentRT 守护进程共享的静态库（目标名 `svc_common`），既是服务框架
兼容层，也是 daemon 层组件集。它屏蔽操作系统差异，为各 daemon 提供统一的服务生命周期、
IPC 通信、JSON-RPC 分发、服务发现、安全认证、容错恢复、并发调度与配置管理等基础设施；
`svc_common` 也是 daemon 与 commons/atoms 之间的依赖枢纽（所有 daemon 均链接它）。

## 架构

```
各 daemon（channel_d / hook_d / sched_d / tool_d / gateway_d / ...）
        │ 链接
        ▼
   svc_common（本模块，静态库）
        │ PUBLIC 传播
        ├─ commons（airy_common：统一基础库）
        ├─ cupolas（安全穹顶，可选）
        ├─ airy_heapstore（运行时数据存储，可选）
        └─ OpenSSL / cJSON / YAML / CURL / Threads（可选）
```

### re-export 兼容层机制（P0.17 / IRON-6）

- 本模块 `include/` 下多个头文件（`svc_common.h` / `circuit_breaker.h` /
  `thread_pool.h` 等）是 **re-export 兼容头**：真实定义已迁移到 commons
  （如 `commons/utils/ipc/include/svc_common.h`、`commons/utils/ipc/include/
  circuit_breaker.h`、`commons/utils/sync/include/thread_pool.h`），兼容头仅
  `#include` 权威版本，使 daemon 源码无需改动包含路径，同时消除编译期
  atoms→daemons 反向依赖（IRON-6 跨层耦合禁令）。
- 其余头（`platform.h` / `compat.h` / `svc_config.h` / `daemon_*` 等）为本模块
  在源码树内的自有/桥接头，仅 daemon 源码树内消费（M0-L5：全部不随库安装，
  权威 API 头由各库自装）。
- CMake 包含路径顺序：commons 权威路径声明在 `daemons/common/include` **之前**，确保
  atoms 代码优先解析 commons 版本。

## 组件集（src/ 共 40 个 C 源文件，0.1.9 0c 迁出 circuit_breaker/thread_pool、移除桩件 daemon_oom）

| 域 | 组件 | 说明 |
|----|------|------|
| 服务框架 | `svc_common` / `svc_registry` / `svc_config` / `svc_monitor` / `svc_client` | 服务生命周期、注册中心客户端、配置加载与监视、监控降级、服务通信客户端 |
| IPC 通信 | `ipc_client` / `ipc_service_bus` / `ipc_bus_helper` / `ipc_backpressure` / `daemon_rpc_client` / `daemon_bootstrap_ipc` | IPC 总线、背压控制、daemon↔daemon 精简 JSON-RPC 客户端、IPC 引导 |
| 服务发现 | `service_discovery` / `service_discovery_lb` / `service_discovery_api` / `service_discovery_stats` / `service_discovery_backend_shm` / `service_discovery_backend_file` / `service_discovery_helper` / `daemon_bootstrap_sd` | 跨进程注册/发现/负载均衡，shm/file 后端，一键引导 |
| JSON-RPC | `jsonrpc_helpers` / `method_dispatcher` | JSON-RPC 2.0 辅助（请求解析/响应构建）与方法分发器（注册表模式，O(1) 路由） |
| 安全 | `svc_auth` / `svc_auth_jwt` / `svc_auth_apikey` / `svc_auth_ratelimit` / `daemon_security` / `param_validator` / `log_sanitizer` | JWT/API Key/限流认证中间件、cupolas 安全集成、参数校验、日志清洗（字符串/路径/URL 安全校验权威实现位于 commons/utils/security） |
| 容错恢复 | `api_recovery` / `alert_manager` | API 恢复策略、智能告警（熔断器 `circuit_breaker` 权威实现已迁 commons/utils/ipc，经 re-export 头接入） |
| 并发调度 | `airy_event_loop` / `daemon_event_driver` / `daemon_task_dispatcher` | 事件循环、统一事件驱动框架（各 daemon 主循环）、工具并行执行引擎（线程池 `thread_pool` 权威实现已迁 commons/utils/sync） |
| 平台兼容 | `platform_compat` | daemon 平台扩展实现（socket/dl/线程名/时间等） |
| 监控指标 | `unified_metrics` | 统一指标采集 |
| 配置 | `config_manager` / `svc_model_defaults` | 统一配置管理、model.yaml 全局默认模型提取（llm_d/gateway_d 共用） |
| 引导 | `daemon_cupolas_bootstrap` / `daemon_heapstore_bootstrap` | cupolas 安全穹顶引导、heapstore 运行时数据存储引导 |
| 其他 | `hall_writer` | daemon 侧事件流写端（hall 事件单一真相源） |

## JSON-RPC 接口表

本模块为**静态库，不暴露独立 JSON-RPC 端点**；它向各 daemon 提供方法分发基础设施：

- `method_dispatcher_create / destroy / register / dispatch`：各 daemon main.c 通过
  `method_dispatcher_register` 注册 `ping` / `health` / `get_stats` / `shutdown` 等
  L2 标准方法（见各 daemon README 的接口表）。
- `jsonrpc_helpers`：`jsonrpc_build_error / jsonrpc_build_success / jsonrpc_parse_request /
  jsonrpc_get_string_param / jsonrpc_get_int_param` 等。
- `daemon_rpc_client`：`daemon_rpc_call`（Unix socket JSON-RPC 客户端，gateway 转发与
  sched_d 真实派发均使用）。

## 配置

- 本模块无独立运行时配置；配置项通过 `daemon_defaults.h` / `svc_config` /
  `config_manager` 暴露给各 daemon。
- Linux 构建时各 daemon 定义 `AIRY_CONFIG_DIR=/etc/agentrt`、`AIRY_LOG_DIR=/var/log/agentrt`
  （macOS 使用 `platform.h` 默认 `./agentrt/config`、`./agentrt/logs`）。

## 依赖与构建

- 必须依赖：`airy_common`（commons，`add_subdirectory` 引入）、`airy_compile_defs`、
  `Threads::Threads`、`airy_platform_libs`。
- 可选依赖（根级探测）：`cupolas`（`if(TARGET cupolas)`，PUBLIC 传播使所有链接 daemon
  自动获得）、`airy_heapstore`（`BUILD_HEAPSTORE`）、OpenSSL、cJSON、YAML、CURL。
- Windows 链接 `ws2_32 bcrypt advapi32`；Unix 定义 `AIRY_PLATFORM_LINUX` / `_GNU_SOURCE`。
- 构建：

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target svc_common
```

- 安装：`svc_common` 静态库 → `lib/`。**不安装兼容层/桥接头（M0-L5，0.1.9）**：
  `include/` 下头仅源码树内消费，权威 API 头由各库自装（corekern/coreloopthree/
  commons 等），避免在 `include/agentrt/` 形成误导性的"第二公共 API 面"。

## 测试

`tests/`（`svc_*` 目标，`BUILD_TESTS` 开启，每个测试注册为 ctest 用例）：

- 基础：`test_error` / `test_platform` / `test_logger` / `test_config` /
  `test_safe_string_utils` / `test_input_validator` / `test_param_validator`
- JSON-RPC/分发：`test_jsonrpc_helpers` / `test_daemon_common`（P1-C06 深度单测）
- 安全：`test_svc_auth` / `test_daemon_security` / `test_log_sanitizer`
- IPC/发现：`test_ipc_service_bus` / `test_ipc_client` / `test_ipc_backpressure` /
  `test_service_discovery`（含 lifecycle/discover/select/health/misc 域拆分文件）
- 容错/并发：`test_strategies_recovery` / `test_api_recovery` / `test_thread_pool` /
  `test_airy_event_loop` / `test_checkpoint` / `test_svc_stop`
- 其他：`test_svc_model_defaults` / `test_hall_writer`

运行：

```bash
ctest --test-dir build -R "svc_test_" -V
```
