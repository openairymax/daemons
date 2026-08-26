# mem_d — Memory 服务守护进程

> 命名空间：`mem.*`
> Unix socket：`${AIRY_RUNTIME_DIR}/mem.sock`（Windows：`\\.\pipe\airy_mem`）
> TCP 端口：8085（`--tcp` 启用，默认仅 Unix socket）

## 定位

`mem_d` 承载 AgentRT 运行时的记忆管理功能，将原
`agentrt/gateway/src/utils/syscall_router.c` 中 `airy_sys_memory_write/search/get/delete`
的进程内联实现抽离为独立 daemon。`gateway_d` 通过 Unix socket IPC 转发
`airy_sys_memory_*` 系统调用请求，在保持 C ABI 兼容的同时获得进程隔离、
独立扩展与故障隔离能力。

除基础记忆读写外，还提供：

- **记忆链（mem.recent）**：最近写入记录倒序返回，供 CLI/TUI 记忆链展示面板使用；
- **记忆演化（mem.evolve）**：L2 标准方法，基于检索/读取/写入组合出"强化记忆"；
- **知识库（mem.kb_\*）**：RAG 一等抽象，按 KB 分组的分块入库、检索、删除与列举；
- **语义缓存（mem.cache_\*，0.1.5）**：跨会话、跨时刻的 LLM 响应复用，L0 精确（SHA-256 键）+ L1 语义（Jaccard 相似度，阈值 0.85）双级命中，LRU + TTL 双淘汰，命中率遥测供 observe_d 聚合；
- **上下文台账（mem.ledger_\*，0.1.5）**：每会话「上下文构成的不可变账本」，append-only 状态流转可审计，预算校验（ctx_budget 默认 32K / warn_ratio 0.8）作为提示词压缩的触发源；
- **提示词压缩（mem.compress，0.1.5）**：L1 规则裁剪（tool_result 截断 / 超轮数丢弃 / 精确去重）+ L2 抽取式摘要（位置加权 + 高频术语 + 指令动词）分层级联，system / tool_def / 当前请求永不压缩，压缩产物与原始条目经台账 ref_id 可回放；
- **混合检索**：自研 TF-IDF 向量余弦相似度 + 可选 embedding 后端，故障自动降级；
- **JSONL 持久化**：所有记录追加写入 `mem.jsonl`，重启后自动加载重建索引。

## 架构

```
+--------------+   airy_sys_memory_*（Unix socket JSON-RPC）   +-----------------+
| gateway_d    | --------------------------------------------> | mem_d           |
| syscall_     |                                               |  +------------+  |
| router.c     |                                               |  | mem_service |  |
+--------------+                                               |  |  - records  |  |
                                                               |  |  - TF-IDF   |  |
                                                               |  |  - JSONL    |  |
                                                               |  +------------+  |
                                                               +-----------------+
```

- 采用 daemon 公共层（`DAEMON_DECLARE_COMMON`）样板：事件驱动（event driver）、
  线程池（min=4 / max=8）、JSON-RPC 分发、`daemon_cupolas_init/cleanup`。
- 服务层 `src/service.c` + `src/mem_svc_adapter.c` 抽为静态库 `airy_mem_service`，
  与 daemon 可执行文件及单元测试共用。
- 检索实现：
  - `src/vector.c`：自研 TF-IDF 词项向量与余弦相似度；
  - `src/emb_client.c`：可选 embedding 后端（基于 libcurl），检索时按
    `tfidf_weight`（默认 0.6）做 TF-IDF 与向量相似度融合；embedding 端点不可达时
    自动降级为纯 TF-IDF，服务不崩溃。
- 持久化：JSONL 文件（`mem.jsonl`），路径由 `airy_data_dir()` 解析，默认落在
  `${AIRY_HOME}/data/agentrt/memory/mem.jsonl`；写记录追加、删记录整文件重写，
  启动时逐行加载并重建记录与 TF-IDF 索引。
- 记录 ID：32 字符十六进制字符串（时间戳 + 计数器 + xorshift 随机）。
- KB 入库：长文本按 `chunk_size` 字节分块（UTF-8 安全切点，默认 512），每块作为
  一条记忆记录写入，metadata 标记 `kb_id` / `doc_id` / `chunk:N`。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `mem.write` | `{data: string, metadata?: object}` | `{record_id}` | 写入一条记忆记录 |
| `mem.search` | `{query: string, limit?: int}` | `{results: [{record_id, score}], total}` | 关键词检索（TF-IDF+embedding 混合，默认 limit=10） |
| `mem.get` | `{record_id: string}` | `{data, length, metadata?}` | 按 ID 读取记录 |
| `mem.delete` | `{record_id: string}` | `{deleted: true}` | 按 ID 删除记录 |
| `mem.count` | `{}` | `{count}` | 当前记录数 |
| `mem.recent` | `{limit?: int}` | `{records: [{record_id, created_at, len, data, metadata?}], total}` | 最近写入记录倒序（新→旧），limit=0 取默认 10 条 |
| `mem.evolve` | `{query?: string, record_id?: string, limit?: int}` | `{status, evolved_record_id, source_count 或 source_record_id}` | 记忆演化：按 query 检索合并命中内容写回强化记录，或按 record_id 读改写带 evolve 元数据；二选一 |
| `mem.kb_ingest` | `{kb_id: string, doc_id?: string, text: string, chunk_size?: int}` | `{kb_id, doc_id?, chunks}` | 文档分块入库（chunk_size=0 取默认 512） |
| `mem.kb_search` | `{kb_id: string, query: string, limit?: int}` | `{results: [{record_id, score}], total}` | 仅在该 KB 内检索 |
| `mem.kb_delete` | `{kb_id: string}` | `{kb_id, deleted_records}` | 删除该 KB 全部记录 |
| `mem.kb_list` | `{}` | `{knowledge_bases: [{kb_id}], total}` | 去重列出所有 KB |
| `mem.cache_put` | `{text, response, model_id, ttl?}` | `{cache_id, exact_key?}` | 写入语义缓存（L0 键 = SHA-256(text\|model_id)） |
| `mem.cache_get` | `{text, model_id, threshold?}` | `{hit, score, cache_id?, response?}` | 查询缓存（L0 精确 → L1 语义） |
| `mem.cache_del` | `{cache_id}` | `{deleted}` | 按 cache_id 删除缓存条目 |
| `mem.cache_stats` | `{}` | `{entries, hits, misses, hit_rate, evictions, bytes}` | 缓存统计 |
| `mem.ledger_append` | `{session_id, entries:[{entry_type, text?, token_in?, token_out?, source?, ref_id?}]}` | `{ledger_id, appended}` | 追加台账条目（append-only） |
| `mem.ledger_window` | `{session_id}` | `{entries[], total_tokens, warn}` | 会话窗口（active 条目 + 预算告警） |
| `mem.ledger_budget` | `{session_id}` | `{used, limit, headroom}` | 会话预算查询 |
| `mem.ledger_mark` | `{session_id, entry_ids[], status}` | `{updated}` | 标记条目状态（追加 status 变更记录，原条目不修改） |
| `mem.ledger_history` | `{session_id, limit?}` | `{events[]}` | 会话历史全量回放（含 status 变更记录） |
| `mem.ledger_stats` | `{}` | `{sessions, entries, total_tokens}` | 台账统计 |
| `mem.compress` | `{session_id, entries:[{entry_id, entry_type, text}]}` | `{context, saved_tokens, actions[], marked}` | 生成压缩计划 + 重组上下文，联动 mark/append |
| `mem.health_check` | `{}` | `{service, healthy, record_count, timestamp}` | 服务健康检查 |
| `mem.get_stats` | `{}` | `{daemon, records, max_records}` | 服务统计 |
| `mem.shutdown` | `{}` | — | 优雅退出 |

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

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

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_MEM_MAX_RECORDS` | 1024 | 最大记录数（< 65536 生效） |
| `AIRY_MEM_TFIDF_WEIGHT` | 0.6 | TF-IDF 与 embedding 相似度的融合权重（0.0–1.0） |
| `AIRY_MEM_EMBEDDING_URL` | 未设置（关闭） | embedding 后端基础 URL（需 libcurl 支持） |
| `AIRY_MEM_EMBEDDING_KEY` | 未设置 | embedding 请求的 Bearer 密钥（可选） |
| `AIRY_MEM_EMB_RETRY_SECONDS` | 60 | embedding 后端故障后的重试间隔 |

## 依赖与构建

依赖：`svc_common`（daemon 公共层）、`airy_common`、cJSON、libyaml（可选）、
libcurl（可选，embedding 后端，由 `AIRY_HAS_CURL` 控制）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target mem_d
```

安装：`cmake --install` 将 `mem_d` 装入 `bin`，头文件装入 `include/agentrt`。

## 测试

```bash
ctest --test-dir build -R mem_d_ --output-on-failure
```

单元测试（链接 `airy_mem_service` 静态库，测试运行时以临时 `AIRY_HOME` /
`AIRY_RUNTIME_DIR` 隔离持久化文件）覆盖：

- `test_service`（记忆引擎）：创建/销毁、写入/读取/删除、容量上限、删除后复用槽位；
  TF-IDF 相关度排序；embedding 降级；持久化一致性；KB 往返与 UTF-8 安全分块；
- `test_cache`（语义缓存）：缓存确定性（两次调用首次 miss 二次 hit 且响应逐字节一致）、
  L0 精确命中、L1 语义阈值边界（0.85 命中 / 未命中 / 显式 1.0 边界）、TTL 过期清理、
  LRU 与字节容量淘汰、模型隔离、命中率统计；
- `test_ledger`（上下文台账）：追加/窗口/预算、append-only 状态流转（mark 压缩后
  history 可回放）、token 一致性（token_standard 确定性）、参数校验；
- `test_compress`（提示词压缩）：L1 确定性（同输入两次结果一致）、保护规则
  （system/tool_def/当前请求完整保留）、超长 tool_result 截断、精确去重、
  超轮数丢弃（保留首轮 + 最近轮）、L2 抽取触发与预算收敛、台账联动回放。
