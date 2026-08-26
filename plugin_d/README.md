# plugin_d — 插件管理守护进程（plugin.* 命名空间）

> **模块路径**: `agentrt/daemons/plugin_d/`
> **命名空间**: `plugin.*`（gateway 转发时剥离前缀，方法名不带 `plugin.`）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/plugin.sock`（TCP 8092 可选，Windows 命名管道 `\\.\pipe\airy_plugin`）
> **插件目录**: `${AIRY_HOME}/ecosystem/plugins`（绝对路径）

## 定位

`plugin_d` 是 AgentRT 的插件管理守护进程，负责动态插件的发现（目录扫描 +
`manifest.yaml` 解析）、权限校验（manifest 权限 → Cupolas 守卫映射）、动态库加载
（dlopen）与生命周期管理（load/unload/start/stop/execute），并与 Cupolas 安全穹顶
集成做加载前强制权限裁决。

## 架构

```
main.c（事件驱动 daemon_event_driver，线程池 4~8、队列 256）
  ├── plugin_discovery    # 扫描 ${AIRY_HOME}/ecosystem/plugins，解析 manifest.yaml
  ├── plugin_permission   # manifest 权限 → safety_guard 守卫类型，逐项裁决
  └── plugin_service      # dlopen/dlsym 加载、状态机、执行、统计
```

- 启动流程：Cupolas 初始化 → 权限模块初始化（strict_mode=true、audit_log=true、
  agent_id="plugin_d"）→ 发现模块初始化（`plugins_dir` 为 `${AIRY_HOME}/ecosystem/
  plugins` 绝对路径，`auto_load=false`、`fail_on_invalid=false`、`scan_depth=1`）→
  扫描并对每个插件做「权限校验 → 加载 → 自动启动」→ 进入事件循环；
- 插件状态机：UNLOADED → LOADED → INITIALIZED → RUNNING → ERROR/DISABLED；
- 权限模块无 Cupolas 上下文（`safety_guard_create` 失败）时降级为本地校验，
  仅记录告警。

## JSON-RPC 接口

以下方法表以 `main.c` 中 `method_dispatcher_register` 的真实注册为准（共 13 个方法）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `load` | `library_path`(必填)、`config_path`(可选) | dlopen 加载插件，返回 `{name}` |
| `unload` | `name`(必填) | 卸载插件，返回 `{unloaded:true}` |
| `start` | `name`(必填) | 启动插件，返回 `{started:true}` |
| `stop` | `name`(必填) | 停止插件，返回 `{stopped:true}` |
| `execute` | `name`(必填)、`input`(必填) | 调用导出入口执行（JSON 入 → 出），返回 `{output}` |
| `get_metadata` | `name`(必填) | 元数据 `{name,version,author,description,type,api_version,min_airy_version}` |
| `get_state` | `name`(必填) | 插件状态 `{state}` |
| `get_stats` | `name`(可选) | 无 `name` 返回 daemon 聚合统计 `{daemon:"plugin_d",plugins,load_total,error_total,memory_bytes}`；有 `name` 返回单插件 `{load_count,error_count,uptime_ns,memory_bytes}` |
| `list` | `type_filter`(可选) | 已加载插件列表 `{plugins:[...],total}` |
| `install` | 同 `load` | `load` 的别名（L2 协议标准方法） |
| `uninstall` | 同 `unload` | `unload` 的别名（L2 协议标准方法） |
| `health_check` | — | `{service:"plugin_d",healthy,plugin_count,timestamp}` |
| `shutdown` | — | 优雅关闭 |

## 权限体系（16 项）

`plugin_permission.c` 的 `SUPPORTED_PERMISSIONS`（manifest 声明 → Cupolas 守卫）：

| 权限 | 守卫类型 | 说明 |
|------|----------|------|
| `file_read` | `SAFETY_GUARD_FILE_READ` | 文件读取 |
| `file_write` | `SAFETY_GUARD_FILE_WRITE` | 文件写入 |
| `network_outbound` | `SAFETY_GUARD_NETWORK` | 出站网络 |
| `network_inbound` | `SAFETY_GUARD_NETWORK` | 入站网络 |
| `tool_execute` | `SAFETY_GUARD_TOOL_EXEC` | 经 tool_d 执行工具 |
| `memory_access` | `SAFETY_GUARD_MEMORY` | 访问记忆 |
| `hook_register` | `SAFETY_GUARD_HOOK` | 注册 Hook |
| `system_call` | `SAFETY_GUARD_SYSTEM` | 系统调用 |
| `process_spawn` | `SAFETY_GUARD_PROCESS` | 派生子进程 |
| `ipc_connect` | `SAFETY_GUARD_IPC` | 连接 IPC 总线 |
| `service_discovery` | `SAFETY_GUARD_SERVICE_DISCOVERY` | 服务发现 |
| `config_read` | `SAFETY_GUARD_CONFIG` | 读配置 |
| `config_write` | `SAFETY_GUARD_CONFIG` | 写配置 |
| `log_write` | `SAFETY_GUARD_LOGGING` | 写日志 |
| `metrics_export` | `SAFETY_GUARD_METRICS` | 导出指标 |
| `audit_trigger` | `SAFETY_GUARD_AUDIT` | 触发审计事件 |

- 严格模式（默认开启）：未声明权限、声明未知权限或任一权限被拒 → 拒绝加载；
- 审计日志（默认开启）：记录 plugin / result / denied 列表。

## 配置

- `plugin_permission_config_t`：`enable_strict_mode=true`、`enable_audit_log=true`、
  `agent_id="plugin_d"`（`safety_policy_path` 未设置）；
- `plugin_discovery_config_t`：`plugins_dir=${AIRY_HOME}/ecosystem/plugins`（绝对
  路径，历史相对路径导致 CWD 漂移扫描为空的问题已修复）、`auto_load=false`、
  `fail_on_invalid=false`、`scan_depth=1`；
- daemon 级：`daemon_parse_args` 支持 `--config`；TCP 8092 需显式启用。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`cupolas`（safety_guard 守卫裁决）、
  `commons` 基础库、libdl（dlopen/dlsym/dlclose）、libyaml（manifest 解析）、
  cJSON；Windows 额外 `ws2_32`；
- 构建产物：可执行文件 `plugin_d`。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target plugin_d
```

## 测试

CTest 测试（`plugin_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `plugin_d_test_plugin_permission` | 权限映射 / 严格模式 / 未知权限裁决 |

```bash
ctest --test-dir build -R "plugin_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
