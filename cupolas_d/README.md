# cupolas_d — Cupolas 安全穹顶守护进程

> 命名空间：`cupolas.*`
> Unix socket：`${AIRY_RUNTIME_DIR}/cupolas.sock`（Windows：`\\.\pipe\airy_cupolas`）
> TCP 端口：8089（`--tcp` 启用，默认仅 Unix socket）

## 定位

`cupolas_d` 将 AgentRT 的 cupolas 安全库（权限引擎 / 输入净化 / 审计日志 /
隔离工位 / 凭据库 / 网络规则 / 授权表）从各 daemon 的内嵌链接
（`svc_common PUBLIC cupolas`）中拆分为可独立部署的守护进程，通过 Unix socket
JSON-RPC 暴露 `cupolas.*` 命名空间方法，使安全能力可供外部组件（如 gateway 编排
分支、运维工具）跨进程调用。

关键能力：

- **进程自身是 cupolas 安全库的宿主**：`main()` 调用 `daemon_cupolas_init("cupolas_d")`
  初始化安全穹顶（permission_engine + sanitizer + audit_logger + daemon_security），
  退出前 `daemon_cupolas_cleanup()` 刷新审计日志并释放资源。
- **方法实现真实调用库 API（IRON-2：无桩）**：全部方法经 `src/service.c` 调用
  `agentrt/cupolas/include/cupolas.h` 公共接口。
- **凭据库（vault_\*）**：凭据加密存取（传输用 hex 编码）、按凭据组轮换
  （默认 round-robin 策略）。
- **网络规则（net_\*）**：防火墙规则管理、按 host/port/protocol/direction 裁决。
- **授权表（entitlements_\*）**：从 YAML 加载授权清单（fs/net/ipc/syscall/
  capability/vault 六类），按 kind/param 裁决。
- **统计为真实计数**：权限裁决 / 输入净化等原子计数器递增，由 `cupolas.get_stats`
  返回。

## 架构

```
调用方 ──(cupolas.* JSON-RPC)──▶ cupolas_d
      │                           ├─ 权限引擎（check_permission / add_rule）
      │                           ├─ 输入净化（sanitize，严格模式 fail-closed）
      │                           ├─ 隔离工位（execute_command）
      │                           ├─ 审计日志（audit_flush）
      │                           ├─ 凭据库（vault_store/retrieve/delete/list/rotate）
      │                           ├─ 网络规则（net_add_rule/check_access/get_stats）
      │                           └─ 授权表（entitlements_load/check）
      └─ daemon_cupolas_init / cleanup（安全穹顶生命周期）
```

- 服务层 `src/service.c` + `src/cupolas_svc_adapter.c` 抽为静态库
  `airy_cupolas_service`，与 daemon 可执行文件共用。
- `cupolas` 是进程级单例库（cupolas_init）：service 实例仅承载统计与配置元数据，
  实际模块初始化由 main() 中的 `daemon_cupolas_init()` 完成。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `cupolas.check_permission` | `{agent_id, action, resource, context?: string}` | `{allowed: bool}` | 权限裁决 |
| `cupolas.sanitize` | `{input}` | `{sanitized}` | 输入净化（严格模式：危险输入拒绝并返回错误） |
| `cupolas.execute_command` | `{command, argv: [string]}` | `{exit_code, stdout, stderr}` | 隔离工位命令执行（返回码与命令退出码一致） |
| `cupolas.add_rule` | `{agent_id?: string, action?: string, resource, allow: bool, priority?: int}` | `{added: bool}` | 动态添加权限规则 |
| `cupolas.audit_flush` | `{}` | `{flushed: true}` | 刷新审计日志 |
| `cupolas.health_check` | `{}` | `{service, healthy, timestamp}` | 服务健康检查 |
| `cupolas.get_stats` | `{}` | 统计 JSON | 真实计数统计（版本/运行时长/裁决数/净化数等） |
| `cupolas.shutdown` | `{}` | — | 优雅退出 |
| `cupolas.vault_store` | `{cred_id, data: hex, type?: int, agent_id?: string}` | `{stored: bool}` | 凭据入库（data 为偶数长度 hex） |
| `cupolas.vault_retrieve` | `{cred_id, agent_id?: string}` | `{data: hex, data_len}` | 按 ID 取回凭据（不存在或无权访问报错） |
| `cupolas.vault_delete` | `{cred_id, agent_id?: string}` | `{deleted: bool}` | 删除凭据 |
| `cupolas.vault_list` | `{type?: int}` | 凭据列表 JSON | 按类型列出凭据 |
| `cupolas.vault_rotate` | `{cred_group, strategy?: int}` | `{selected_id}` | 按组轮换凭据（默认策略 round-robin） |
| `cupolas.net_add_rule` | `{rule_id, src_ip?, dst_ip?, src_port?, dst_port?, protocol?: int, direction?: int, action?: int, priority?: int, description?: string}` | `{added: bool}` | 添加网络规则 |
| `cupolas.net_check_access` | `{host, port: int(0-65535), protocol?: int, direction?: string}` | `{allowed: bool}` | 网络访问裁决（拒绝返回 AIRY_ERR_PERMISSION_DENIED 时同样返回 allowed=false） |
| `cupolas.net_get_stats` | `{}` | 网络统计 JSON | 网络规则统计 |
| `cupolas.entitlements_load` | `{yaml_path}` | `{loaded: bool}` | 从 YAML 加载授权清单 |
| `cupolas.entitlements_check` | `{kind, param1, param2?: string}` | `{allowed: bool}` | 授权裁决（kind ∈ fs/net/ipc/syscall/capability/vault） |

所有方法在服务未就绪时返回内部错误，不产生部分副作用。

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/cupolas.sock",
    "tcp_port": 8089,
    "max_clients": 64
  }
}
```

## 依赖与构建

依赖：`cupolas` 安全库（`agentrt/cupolas`）、`svc_common`（daemon 公共层）、
`airy_common`、cJSON、libyaml（授权表解析）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target cupolas_d
```

安装：`cmake --install` 将 `cupolas_d` 装入 `bin`，头文件装入 `include/agentrt`。

## 测试

本模块当前无单元测试目录（`cupolas.*` 各方法的库级测试见 `agentrt/cupolas`
模块自身的测试）。

启动验证：

```bash
./bin/cupolas_d --manager cupolas.json &
# 或直接以 Unix socket 验证：
python3 - <<'EOF'
import json, socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/agentrt/cupolas.sock")
req = {"jsonrpc": "2.0", "id": 1, "method": "cupolas.check_permission",
       "params": {"agent_id": "agent_1", "action": "read", "resource": "/data/notes"}}
s.sendall(json.dumps(req).encode())
print(s.recv(4096))
EOF
```
