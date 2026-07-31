# mem_d — Memory 服务守护进程

> **命名空间**：`mem.*`
> **Unix socket**：`${AIRY_RUNTIME_DIR}/mem.sock`（默认 `/tmp/agentrt/mem.sock`）
> **TCP 端口**：8085（可选，`--tcp` 启用）

## 职责

`mem_d` 承载 AgentRT 运行时记忆管理功能，从原 `agentrt/gateway/src/utils/syscall_router.c`
的 `airy_sys_memory_write/search/get/delete` 进程内联实现抽离为独立 daemon。

`gateway_d` 通过 Unix socket IPC 转发 `airy_sys_memory_*` 系统调用请求到 `mem_d`，
保持 C ABI 兼容的同时获得进程隔离、独立扩展与故障隔离能力。

## JSON-RPC 方法

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `mem.write` | `{data: string, metadata?: object}` | `{record_id: string}` | 写入记忆记录 |
| `mem.search` | `{query: string, limit?: int}` | `{results: [{record_id, score}], total: int}` | 关键词检索 |
| `mem.get` | `{record_id: string}` | `{data: string, length: int, metadata?: string}` | 按 ID 读取 |
| `mem.delete` | `{record_id: string}` | `{deleted: bool}` | 按 ID 删除 |
| `mem.count` | `{}` | `{count: int}` | 当前记录数 |

## 架构

```
+--------------+       airy_sys_memory_*        +-----------------+
| gateway_d    |  --- (thin IPC client) --->    | mem_d           |
| syscall_     |       Unix socket              |  +------------+  |
| router.c     |  /var/run/agentrt/mem.sock    |  | mem_service|  |
+--------------+                                |  |  - records |  |
                                                |  |  - hash    |  |
                                                |  +------------+  |
                                                +-----------------+
```

## 内部数据结构

- `records[]`：固定容量数组（默认 1024，可通过 `AIRY_MEM_MAX_RECORDS` 或配置文件 `daemon.max_records` 覆盖）
- `record_index`：djb2 哈希表，`record_id → records[]` 索引
- 记录 ID：32 字符十六进制字符串（时间戳 + 计数器 + xorshift 随机）

## 检索策略

无嵌入模型时的退化检索：基于子串匹配计数 + 密度归一化评分（`[0.0, 1.0]`）。
后续可扩展为接入向量检索（如 FAISS / 自研 embedding daemon）。

## 配置

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/mem.sock",
    "tcp_port": 8085,
    "max_clients": 64,
    "max_records": 1024
  }
}
```

环境变量：
- `AIRY_MEM_MAX_RECORDS`：最大记录数（覆盖默认值 1024）

## 构建

```bash
cd build && cmake .. && make mem_d
```

## 测试

```bash
cd build && ctest -R mem_d_ --verbose
```
