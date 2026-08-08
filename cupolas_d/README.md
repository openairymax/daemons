# cupolas_d — Cupolas 安全穹顶独立 daemon

`cupolas_d` 将 AgentRT 的 cupolas 安全库（权限引擎 / 输入净化 / 审计日志 / 隔离工位）从
各 daemon 的内嵌链接（`svc_common PUBLIC cupolas`）中拆分为可独立部署的守护进程，
通过 Unix socket JSON-RPC 暴露 `cupolas.*` 命名空间方法。

## 设计

- 方法实现全部真实调用 `agentrt/cupolas/include/cupolas.h` 公共 API（IRON-2：无桩）。
- 进程自身是 cupolas 安全库的宿主：`main()` 调用 `daemon_cupolas_init("cupolas_d")`
  初始化安全穹顶（permission_engine + sanitizer + audit_logger + daemon_security），
  退出前 `daemon_cupolas_cleanup()` 刷新审计日志并释放资源。
- 统计为真实计数：每次权限裁决 / 输入净化都会递增原子计数器，由 `cupolas.get_stats` 返回。
- 服务/适配器结构：`src/service.c`（cupolas_service_t 实现）+
  `src/cupolas_svc_adapter.c`（适配到 `airy_svc_t` 统一服务管理框架），
  参照 `mem_d` 同构实现。

## 构建

```bash
cd agentrt-build
cmake ..
cmake --build . --target cupolas_d -j$(nproc)
```

二进制输出至 `agentrt-build/bin/cupolas_d`，`cmake --install` 时安装到 `$AIRY_HOME/bin`。

## 启动

```bash
cd /home/spharx/SpharxWorks/.airymaxrt
nohup ./bin/cupolas_d > logs/cupolas_d.log 2>&1 &
```

Unix socket 路径：`$AIRY_RUNTIME_DIR/cupolas.sock`（默认 `${AIRY_HOME}/run/cupolas.sock`）。
支持 `--manager <config>`（JSON 配置，`daemon.socket_path/tcp_port/max_clients`）与 `--tcp` 选项。

## JSON-RPC 方法

| 方法 | 参数 | 结果 |
|------|------|------|
| `cupolas.check_permission` | `{agent_id, action, resource, context?}` | `{allowed: bool}` |
| `cupolas.sanitize` | `{input}` | `{sanitized: string}` |
| `cupolas.execute_command` | `{command, argv: [string]}` | `{exit_code, stdout, stderr}` |
| `cupolas.add_rule` | `{agent_id?, action?, resource, allow, priority}` | `{added: bool}` |
| `cupolas.audit_flush` | `{}` | `{flushed: true}` |
| `cupolas.get_stats` | `{}` | `{daemon, version, uptime_s, permission_checks, sanitize_count}` |
| `cupolas.shutdown` | `{}` | `{status: "shutting_down"}`（优雅退出） |
| `cupolas.health_check` | `{}` | `{service, healthy, timestamp}` |

### 调用示例（Python）

```python
import json, socket

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/home/spharx/SpharxWorks/.airymaxrt/run/cupolas.sock")
req = {"jsonrpc": "2.0", "id": 1, "method": "cupolas.check_permission",
       "params": {"agent_id": "agent_1", "action": "read", "resource": "/data/notes"}}
s.sendall(json.dumps(req).encode())
resp = b""
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    resp += chunk
    try:
        print(json.loads(resp))
        break
    except json.JSONDecodeError:
        continue
```

## 安全语义

- 输入净化采用严格模式（SANITIZE_LEVEL_STRICT）：危险输入被拒绝（fail-closed），
  返回 JSON-RPC 错误；可净化输入返回净化产物。
- 命令执行经 cupolas workbench 隔离，输出缓冲 64KB。
- 所有方法在服务未就绪时返回内部错误，不产生部分副作用。
