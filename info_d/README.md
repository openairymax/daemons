# info_d — 系统信息守护进程

> 命名空间：无前缀（gateway_d 转发时剥离 `<daemon>.` 前缀，daemon 内部按裸方法名分发）
> Unix socket：`${AIRY_RUNTIME_DIR}/info.sock`（Windows：`127.0.0.1:8088` TCP）
> TCP 端口：8088（仅 Windows 默认使用 TCP；POSIX 默认 Unix socket）

## 定位

`info_d` 是 AgentRT 的系统信息采集守护进程：周期采集 CPU、内存、磁盘、系统
运行时长等资源指标，通过 JSON-RPC 2.0 对外提供实时与历史快照查询，是调度、
监控、告警等上层服务的基础数据源。

关键能力：

- **周期性采集**：后台采集线程每 5s 生成一次系统快照，维护最新快照 +
  64 条环形缓冲历史（`INFO_D_HISTORY_SIZE`）。
- **跨平台采集**：Linux 经 `sysinfo`/`statvfs`/`sysconf`/`uname`；macOS 经
  `sysctl`/`host_statistics64`（`KERN_BOOTTIME`）；Windows 经
  `GlobalMemoryStatusEx`/`GetDiskFreeSpaceExW`。
- **健康检测**：最近采集时间超过 3 个采集间隔（15s）判定为 `degraded`。
- **自定义事件循环**：不使用 `DAEMON_DECLARE_COMMON` 样板，采用自定义
  `airy_event_loop`（epoll）+ 采集线程，请求处理在事件循环内同步完成。

## 架构

```
调用方 ──(JSON-RPC 2.0 / Unix socket)──▶ info_d
      │                                   ├─ 采集线程（每 5s 快照 → 环形历史 64 条）
      │                                   ├─ airy_event_loop（epoll）请求分发
      │                                   └─ L2 方法：system / history / health /
      │                                      health_check / get_stats / shutdown
      └─ 非 JSON-RPC 请求 → 返回自描述 JSON 字符串（旧格式兼容）
```

- 单文件实现（`src/main.c`），无独立 include 目录。
- 快照字段：`cpu_cores`、`cpu_usage_pct`、`total/free/used_memory_kb`、
  `memory_usage_pct`、`total/free/used_disk_kb`、`disk_usage_pct`、`uptime_sec`、
  `timestamp`。
- 方法分发：先尝试按 JSON-RPC 2.0 解析；命中 L2 标准方法（`shutdown` /
  `get_stats` / `health_check`）或自定义方法（`system` / `history` / `health`）
  按 JSON-RPC 响应；其余请求保持旧格式——返回内联 `system` + `collection`
  的自描述 JSON 字符串（向后兼容）。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `system` | `{}` | `{service, platform, hostname, kernel_version, system: {…快照…}}` | 当前系统信息快照 |
| `history` | `{N?: int}`（也接受 `n`/`count`/`limit` 或数组首个元素） | `[快照…]` | 环形历史快照列表（最旧→最新；N=0 返回空数组，上限 64） |
| `health` | `{}` | `{status, service, collecting, running, last_collect_time, staleness_sec, uptime_s, timestamp}` | 采集服务健康状态（status ∈ ok/degraded） |
| `health_check` | `{}` | `{status: "ok", service, uptime_s, timestamp}` | L2 标准健康检查（无副作用） |
| `get_stats` | `{}` | `{daemon, uptime_s, requests, errors, history_count}` | 服务统计 |
| `shutdown` | `{}` | `{status: "shutting_down"}` | 优雅停止（同步停止事件循环后进程退出） |

## 配置

本 daemon 不读取配置文件（`main()` 忽略 argc/argv），以下为编译期常量：

| 常量 | 值 | 说明 |
|------|-----|------|
| `INFO_D_DEFAULT_PORT` | 8088 | TCP 端口（仅 Windows 默认使用） |
| `INFO_D_DEFAULT_SOCKET` | `${AIRY_RUNTIME_DIR}/info.sock` | Unix socket 路径 |
| `INFO_D_COLLECT_INTERVAL_SEC` | 5 | 采集间隔（秒） |
| `INFO_D_HISTORY_SIZE` | 64 | 环形历史容量 |
| `INFO_D_MAX_BUFFER` | 65536 | 请求缓冲上限 |

信号：`SIGINT`/`SIGTERM` 优雅关闭；`SIGUSR1` 切换日志级别（INFO ↔ DEBUG）；
`SIGPIPE` 忽略。

## 依赖与构建

依赖：`airy_common`、`svc_common`（daemon 公共层）、`Threads::Threads`、
`airy_platform_libs`（Windows 另需 `ws2_32`）、cJSON。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target info_d
```

安装：`cmake --install` 将 `info_d` 装入 `bin`。

## 测试

```bash
ctest --test-dir build -R info_ --output-on-failure
```

单元测试（`tests/test_info_service.c`，将 `src/main.c` 直接编译进测试单元、
`main()` 重命名规避入口冲突）覆盖：

- 环形历史缓冲：追加/回绕/容量上限，`history` 按 `N` 截取（对象 `N`/`count`
  或数组参数）；
- `system`/`history`/`health` 方法响应结构与字段完整性；
- 请求分发：JSON-RPC `system` 方法命中返回 `result.system`；未知方法回退旧格式
  （返回内联 `system` 字段）。
