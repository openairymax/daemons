# examples — 守护进程服务框架使用示例

## 定位

`examples/` 不是守护进程模块，而是 AgentRT 服务管理框架（`svc_common.h`）的
**使用示例**：演示如何用 `airy_svc_*` API 实现一个服务的完整生命周期
（创建 → 初始化 → 启动 → 状态查询 → 健康检查 → 统计 → 暂停/恢复 → 停止 → 销毁）。

> 本目录**不参与顶层构建**（`daemons/CMakeLists.txt` 未 `add_subdirectory(examples)`），
> 需手动编译运行。

## 架构

`example_svc_usage.c` 定义示例服务上下文（`example_service_context_t`），实现
`airy_svc_interface_t` 五个回调（`init` / `start` / `stop` / `destroy` /
`healthcheck`），再通过服务管理框架 API 驱动生命周期，并在关键步骤打印状态
（`airy_svc_state_to_string` / `airy_svc_is_ready` / `airy_svc_is_running`）。

示例服务配置：

- 名称 `example-service`，版本 `0.1.1`；
- 能力：`AIRY_SVC_CAP_ASYNC`（异步）| `AIRY_SVC_CAP_PAUSEABLE`（可暂停）；
- `max_concurrent=10`、`timeout_ms=5000`、`priority=5`、`auto_start=true`、
  `enable_metrics=true`、`enable_tracing=false`。

演示步骤与所用 API：

| 步骤 | API | 说明 |
|------|-----|------|
| 1 | `airy_svc_create()` | 创建服务实例（名称/接口/配置） |
| 2 | `airy_svc_init()` | 初始化，分配服务上下文 |
| 3 | `airy_svc_start()` | 启动服务 |
| 4 | `airy_svc_get_name/get_version/get_state/is_ready/is_running` | 查询服务状态 |
| 5 | `airy_svc_healthcheck()` | 健康检查 |
| 6 | `airy_svc_get_stats()` | 获取统计（request/success/error 计数） |
| 7 | `airy_svc_has_capability()` + `airy_svc_pause()` / `airy_svc_resume()` | 暂停/恢复 |
| 8 | `airy_svc_stop()` | 停止（失败时强制停止） |
| 9 | `airy_svc_destroy()` | 销毁实例 |
| 10 | `airy_svc_count()` | 注册表中的服务数量 |

## 依赖与构建

依赖：`svc_common.h`（`daemons/common/include`）、`libsvc_common`、
`libairy_common`、pthread。

```bash
# Linux/macOS（以仓库内路径为例）
gcc -o example_svc_usage example_svc_usage.c \
    -I./common/include \
    -L./build/daemons/common \
    -lsvc_common -lairy_common -lpthread

# Windows
cl example_svc_usage.c /I./common/include /link svc_common.lib airy_common.lib
```

## 运行

```bash
./example_svc_usage
```

程序依次演示 1–10 步生命周期操作，每步打印当前服务状态，最终输出
"示例程序完成"。

## 测试

本目录为示例代码，无单元测试。
