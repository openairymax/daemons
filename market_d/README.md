# market_d — 应用市场守护进程（market.* 命名空间）

> **模块路径**: `agentrt/daemons/market_d/`
> **命名空间**: `market.*`（gateway 转发时剥离前缀，方法名不带 `market.`）
> **默认监听**: Unix socket `${AIRY_RUNTIME_DIR}/market.sock`（TCP 8082 可选，Windows 命名管道 `\\.\pipe\airy_market`）

## 定位

`market_d` 是 AgentRT 的应用市场守护进程，负责 Agent 与 Skill 两类资源的注册、
搜索、安装与发布管理，是 Agent/Skill 从开发到部署的流转枢纽。默认数据落到
`$AIRY_HOME/agents` 与 `$AIRY_HOME/skills`，支持远程注册中心同步（默认关闭）。

## 架构

```
客户端 (JSON-RPC 2.0 over Unix socket / TCP)
        ↓
  main.c（线程池 accept 循环，4~8 线程、队列 256 + 方法分发）
        ↓
  market_service（注册 / 搜索 / 安装 / 发布）
        ├── agent_registry_core / agent_registry   # Agent 注册管理
        ├── skill_registry                          # Skill 注册管理
        ├── installer.c                             # 安装器（版本/强制更新/安装路径）
        ├── publisher.c                             # 资源发布
        └── market_service_config.c                 # 配置加载
```

- 服务启动时使用内建默认配置（`registry_url=NULL`、`storage_path=NULL` → 回退
  `$AIRY_HOME/agents` 或 `$AIRY_HOME/skills`，`sync_interval_ms=30000`、
  `cache_ttl_ms=3600000`、`enable_remote_registry=false`、`enable_auto_update=false`）；
- `publish` 复用安装逻辑：接受 `params.agent` 或 `params.skill` 对象，持久化到
  `$AIRY_HOME/agents`（或 `/skills`）并返回安装路径；
- `search_agents` 结果中 `installed` 字段 = 状态为 `AGENT_STATUS_AVAILABLE`。

## JSON-RPC 接口

以下方法表以 `main.c` 中 `method_dispatcher_register` 的真实注册为准（共 11 个方法）：

| 方法 | 参数（params） | 说明 |
|------|----------------|------|
| `register_agent` | `agent`(对象，必填；`agent_id` 必填，`name`/`version`/`description`/`author` 可选) | 注册 Agent，返回 `{status:"registered",agent_id}` |
| `search_agents` | `keyword`(默认 "")、`offset`(默认 0)、`limit`(默认 20) | 搜索 Agent，返回数组（含 `installed` 布尔） |
| `install_agent` | `agent_id`(必填)、`version`(默认 "latest")、`install_path`(可选)、`force_update`(可选) | 安装 Agent，返回 `{status,agent_id,installed_version,message?,install_path?}` |
| `register_skill` | `skill`(对象，必填；`skill_id` 必填，`name`/`version` 可选) | 注册 Skill，返回 `{status:"registered",skill_id}` |
| `search_skills` | `keyword`(默认 "") | 搜索 Skill（limit 固定 20、offset 0） |
| `publish` | `agent` 或 `skill`(对象，必填)、`version`(默认 "latest")、`install_path`/`force_update`(可选) | 发布（落盘）Agent/Skill，返回 `{status:"published",type,id,published_version,message?,install_path?}` |
| `search` | 同 `search_agents` | `search_agents` 的别名（L2 协议标准方法） |
| `install` | 同 `install_agent` | `install_agent` 的别名（L2 协议标准方法） |
| `health_check` | — | `{service:"market_d",healthy:true,timestamp}` |
| `get_stats` | — | `{daemon:"market_d",agents,skills,installed_agents,installed_skills}` |
| `shutdown` | — | 优雅关闭 |

## 配置

- 配置：默认使用内置 `market_config_t` 默认值（数据落盘 `$AIRY_HOME/agents`
  与 `$AIRY_HOME/skills`），可经 `--config <path>` 覆盖（YAML）；仓库内
  不随附 market_d 默认配置文件（S-7 收敛：不再引用已废弃的
  `agentrt/manager/service/` 相对布局）；
- `market_config_t`：`registry_url`、`storage_path`、`sync_interval_ms`、
  `cache_ttl_ms`、`enable_remote_registry`、`enable_auto_update`；
- 数据落盘：`$AIRY_HOME/agents`（Agent）与 `$AIRY_HOME/skills`（Skill）。

## 依赖与构建

- 依赖：`svc_common`（daemons/common）、`commons` 基础库、cJSON、线程池；
- 构建产物：可执行文件 `market_d`（含 `market_svc_adapter.c` 适配层）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target market_d
```

## 测试

CTest 测试（`market_d_*`）：

| 测试 | 覆盖点 |
|------|--------|
| `market_d_test_agent_registry` | Agent 注册 |
| `market_d_test_skill_registry` | Skill 注册 |
| `market_d_test_installer` | 安装器 |

测试数据：`tests/agents/install_test_agent/agent.json`。

```bash
ctest --test-dir build -R "market_d_" -V
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
