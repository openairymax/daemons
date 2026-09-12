# cupolas_d — 安全穹顶守护进程

> **模块路径**：`agentrt/daemons/cupolas_d/` · **可执行文件 / CMake 目标**：`cupolas_d` · **RPC 命名空间**：`cupolas.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`cupolas_d` 把 cupolas 安全能力（权限引擎、输入净化、审计日志、隔离工位、凭据库、
网络规则、授权表、动态策略）封装成可独立部署的常驻进程，通过 JSON-RPC 对外提供
安全裁决与凭据服务。各守护进程既可以内嵌链接 cupolas 库，也可以跨进程调用本服务，
两者共用同一套库实现。

- 端点：POSIX Unix socket `<runtime-dir>/cupolas.sock`（`$AIRY_HOME/run/cupolas.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8089`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8089`（默认只监听 socket）。

## 能力

- **权限裁决** — `check_permission` 按 agent / action / resource 判定，`add_rule` 动态加规则。
- **输入净化** — 严格模式 fail-closed：危险输入直接返回错误而不是放行。
- **隔离工位** — `execute_command` 在受控工位执行命令，返回码与命令退出码一致。
- **凭据库** — 凭据加密存取（传输层 hex 编码）、按 ID 取回、列举、删除、按组轮换。
- **网络规则** — 按 host / port / protocol / direction 裁决出站访问，附带规则统计。
- **授权表** — 从 YAML 载入 fs / net / ipc / syscall / capability / vault 六类授权并裁决。
- **动态策略（PDP）** — 策略文档两段式生效：先 `policy_load` 入暂存集并做冲突检测，
  再 `policy_activate` 递增 epoch 并向 PEP 广播；支持按版本回滚与状态查询。
- **真实统计** — 权限裁决、输入净化等计数为原子计数器真实值。

## 架构

```
调用方 ──(cupolas.* JSON-RPC)──▶ cupolas_d
      │                           ├─ 权限引擎（check_permission / add_rule）
      │                           ├─ 输入净化（sanitize，严格模式 fail-closed）
      │                           ├─ 隔离工位（execute_command）
      │                           ├─ 审计日志（audit_flush）
      │                           ├─ 凭据库（vault_store / retrieve / delete / list / rotate）
      │                           ├─ 网络规则（net_add_rule / check_access / get_stats）
      │                           ├─ 授权表（entitlements_load / check）
      │                           └─ 动态策略（policy_load / activate / rollback / status）
      └─ daemon_cupolas_init / cleanup（安全穹顶生命周期）
```

- 进程自身是 cupolas 库的宿主：`main()` 调用 `daemon_cupolas_init("cupolas_d")` 完成
  permission engine + sanitizer + audit logger 初始化，退出前 `daemon_cupolas_cleanup()`
  刷新审计并释放资源。
- 服务层 `src/service.c` + `src/cupolas_svc_adapter.c` 抽为静态库 `airy_cupolas_service`。
- RPC 处理按域拆分：`cupolas_rpc_core.c`（权限/净化/命令/规则/审计/健康/统计）、
  `cupolas_rpc_vault.c`（凭据，含 hex 编解码）、`cupolas_rpc_net_entitlements.c`
  （网络规则与授权表）、`cupolas_rpc_policy.c`（动态策略）。
- 方法经 `main.c` 内的 `REG_RPC` 宏注册进同一个 dispatcher。

## JSON-RPC 接口

共 22 个方法（方法名不含命名空间前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `check_permission` | `{agent_id, action, resource, context?: string}` | `{allowed: bool}` | 权限裁决 |
| `add_rule` | `{agent_id?, action?, resource, allow: bool, priority?: int}` | `{added: bool}` | 动态添加权限规则 |
| `sanitize` | `{input}` | `{sanitized}` | 输入净化（危险输入返回错误） |
| `execute_command` | `{command, argv: [string]}` | `{exit_code, stdout, stderr}` | 隔离工位命令执行 |
| `audit_flush` | `{}` | `{flushed: true}` | 刷新审计日志 |
| `vault_store` | `{cred_id, data: hex, type?: int, agent_id?: string}` | `{stored: bool}` | 凭据入库（`data` 为偶数长度 hex） |
| `vault_retrieve` | `{cred_id, agent_id?: string}` | `{data: hex, data_len}` | 按 ID 取回凭据 |
| `vault_delete` | `{cred_id, agent_id?: string}` | `{deleted: bool}` | 删除凭据 |
| `vault_list` | `{type?: int}` | 凭据列表 JSON | 按类型列出凭据 |
| `vault_rotate` | `{cred_group, strategy?: int}` | `{selected_id}` | 按组轮换凭据（默认 round-robin） |
| `net_add_rule` | `{rule_id, src_ip?, dst_ip?, src_port?, dst_port?, protocol?: int, direction?: int, action?: int, priority?, description?}` | `{added: bool}` | 添加网络规则 |
| `net_check_access` | `{host, port: int(0-65535), protocol?: int, direction?: string}` | `{allowed: bool}` | 网络访问裁决 |
| `net_get_stats` | `{}` | 网络统计 JSON | 网络规则与命中统计 |
| `entitlements_load` | `{yaml_path}` | `{loaded: bool}` | 从 YAML 载入授权清单 |
| `entitlements_check` | `{kind, param1, param2?}` | `{allowed: bool}` | 授权裁决（kind ∈ fs/net/ipc/syscall/capability/vault） |
| `policy_load` | `{json: string}` | `{staged: true, rule_count, conflict_count, strategy, epoch, conflicts: [{rule_a, rule_b, reason}]}` | 策略文档入暂存集并做冲突检测，**不改变运行裁决** |
| `policy_activate` | `{description?: string}` | `{epoch, version_count, rule_count}` | 将暂存集生效为新版本，`epoch` 递增并广播 PEP |
| `policy_rollback` | `{version: int}` | `{epoch, version_count, rule_count}` | 回滚到指定历史版本 |
| `policy_status` | `{}` | `{enabled, epoch, version_count, rule_count, staged, staged_rule_count, strategy, max_versions}` | 策略引擎状态 |
| `health_check` | `{}` | `{service, healthy, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | 统计 JSON | 版本 / 运行时长 / 裁决数 / 净化数等真实计数 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`cupolas.check_permission`），由 `gateway_d`
剥离前缀后转发。

策略生效是两段式的：`policy_load` 只暂存并做规则冲突检测，随后 `policy_activate` 才递增
`epoch` 并使新规则参与裁决；无暂存内容时 `policy_activate` 返回 `-32602`，
`policy_rollback` 指定的版本不存在同样返回 `-32602`。服务未就绪时方法返回内部错误，
不产生部分副作用。

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/cupolas.sock",
    "tcp_port": 8089,
    "max_clients": 64
  }
}
```

命令行参数：`--manager <config>`、`--tcp`、`--help`。

环境变量：`AIRY_CUPOLAS_SOCK` 用于在网关侧覆盖本进程端点。

## 用法

```bash
airymaxrt logs cupolas_d       # 运行态日志
<build-dir>/bin/cupolas_d      # 手工启动单个进程（监听运行目录）
```

直连验证（POSIX）：

```bash
python3 - <<'EOF'
import json, os, socket
path = os.path.join(os.environ.get("AIRY_HOME", os.path.expanduser("~/.airy")), "run", "cupolas.sock")
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(path)
req = {"jsonrpc": "2.0", "id": 1, "method": "check_permission",
       "params": {"agent_id": "agent_1", "action": "read", "resource": "/data/notes"}}
s.sendall(json.dumps(req).encode())
print(s.recv(4096))
EOF
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target cupolas_d
```

`cmake --install` 将 `cupolas_d` 装入 `bin`，`include/` 下的头装入 `include/agentrt`。
Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 依赖

| 依赖 | 用途 |
|------|------|
| [cupolas](https://atomgit.com/openairymax/cupolas) | 权限引擎、净化、审计、隔离工位、凭据库、网络规则、授权表、动态策略 |
| [`svc_common`](../common/README.md) | 生命周期状态机、事件驱动、JSON-RPC dispatcher |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON 封装 |
| [corekern](https://atomgit.com/openairymax/atoms) | `airy_init()` 核心引导 |
| libyaml | 授权表 YAML 解析 |
| [gateway_d](../gateway_d/README.md) | 上游：`cupolas.*` 命名空间转发方 |

## 关系

其它 daemon 通过 `svc_common` 内嵌链接 cupolas 库完成本地裁决；`cupolas_d` 面向的是
**跨进程**场景——网关编排、运维工具、外部组件需要复用同一份策略与凭据视图时调用本服务。
`gateway_d` 的策略接口 `policy.*` 亦转发到此处。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
