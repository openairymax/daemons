# mem_d — 记忆服务守护进程

> **模块路径**：`agentrt/daemons/mem_d/` · **可执行文件 / CMake 目标**：`mem_d` · **RPC 命名空间**：`mem.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`mem_d` 是 AgentRT 运行时的记忆服务，以独立进程提供长期记忆的写入、检索与治理。
它把「记什么、怎么找回来、上下文装不下时怎么办」这三件事收在一处：基础记忆读写、
知识库分块入库、跨会话语义缓存、每会话上下文台账与提示词压缩。

- 端点：POSIX Unix socket `<runtime-dir>/mem.sock`（`$AIRY_HOME/run/mem.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8085`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8085`（默认只监听 socket）。
- 持久化：`${AIRY_DATA_DIR}/agentrt/memory/mem.jsonl`（行分隔 JSON，整文件重写走
  临时文件 + rename，崩溃安全）。

## 能力

- **基础记忆读写** — 写入 / 检索 / 读取 / 删除 / 计数，记录 ID 为 32 字符十六进制串。
- **混合检索** — 自研 TF-IDF 词项向量 + 余弦相似度为基线，配置 embedding 后端后按
  `tfidf_weight` 融合两路分数；embedding 端点不可达时自动降级为纯 TF-IDF。
- **记忆链与演化** — `recent` 倒序返回最近记录；`evolve` 组合检索 / 读取 / 写入产出
  一条带演化元数据的新记录。
- **知识库（RAG）** — 长文档按字节分块入库（UTF-8 安全切点，默认 512 字节），
  块内记录携带 `kb_id` / `doc_id` / `chunk:N` 元数据，可按 KB 独立检索与删除。
- **语义缓存** — L0 精确命中（SHA-256 键）+ L1 语义命中（Jaccard 相似度，阈值 0.85），
  LRU 与 TTL 双淘汰，命中率可遥测。
- **上下文台账** — 每会话一条 append-only 的上下文构成账本，状态流转只追加不改写，
  预算（默认 32768 token、告警比例 0.8）作为压缩触发源。
- **提示词压缩** — L1 规则裁剪（超长 tool_result 截断 / 超轮数丢弃 / 精确去重）+
  L2 抽取式摘要（位置加权 + 高频术语 + 指令动词）分层级联；`system`、`tool_def`
  与当前请求永不压缩，产物与原始条目通过台账 `ref_id` 可回放。

## 架构

```
gateway_d ──(mem.write / mem.search / mem.kb_ingest / …)──▶ mem_d
                                                            │
                     ┌──────────────────────────────────────┤
                     ▼                                      ▼
              src/core/service.c                    src/handlers/*.c
              记录表 + 索引 + 生命周期               JSON-RPC 参数编解码
                     │                                      │
        ┌────────────┼───────────────┐                      ▼
        ▼            ▼               ▼               src/core/main.c
   src/engine/  src/engine/     src/engine/          25 个方法注册
   vector.c     kb.c            cache.c / ledger.c
   TF-IDF       UTF-8 分块      / compress.c
                                          │
                                          ▼
                                src/engine/mem_persist.c
                                JSONL 追加 + 索引重建
```

- 服务层 `src/core/service.c` + `src/core/mem_svc_adapter.c` 等抽为静态库
  `airy_mem_service`，被守护进程可执行文件与单元测试共用；
- 采用 daemon 公共层样板（`DAEMON_DECLARE_COMMON`）：事件驱动、JSON-RPC 分发、
  cupolas 安全穹顶初始化；
- 全局单例容量：记录表 1024（可配）、缓存 4096 条 / 64 MiB / TTL 1 小时、
  台账 1024 会话 / 200000 条目。

## JSON-RPC 接口

共 25 个方法，经 `method_dispatcher_register` 注册（下表方法名不带 `mem.` 前缀）：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `write` | `{data: string, metadata?: object}` | `{record_id}` | 写入一条记忆记录 |
| `search` | `{query: string, limit?: int}` | `{results: [{record_id, score}], total}` | 混合检索（默认 `limit=10`） |
| `get` | `{record_id}` | `{data, length, metadata?}` | 按 ID 读取 |
| `delete` | `{record_id}` | `{deleted: true}` | 按 ID 删除 |
| `count` | `{}` | `{count}` | 当前记录数 |
| `recent` | `{limit?: int}` | `{records: [{record_id, created_at, len, data, metadata?}], total}` | 最近写入倒序（`limit=0` 取默认 10） |
| `evolve` | `{query?: string, record_id?: string, limit?: int}` | `{status, evolved_record_id, source_count 或 source_record_id}` | 记忆演化（按 query 或按 record_id，二选一） |
| `kb_ingest` | `{kb_id, text, doc_id?, chunk_size?: int}` | `{kb_id, doc_id?, chunks}` | 文档分块入库（`chunk_size=0` 取 512） |
| `kb_search` | `{kb_id, query, limit?: int}` | `{results: [{record_id, score}], total}` | 仅在指定 KB 内检索 |
| `kb_delete` | `{kb_id}` | `{kb_id, deleted_records}` | 删除该 KB 全部记录 |
| `kb_list` | `{}` | `{knowledge_bases: [{kb_id}], total}` | 列出去重后的 KB |
| `cache_put` | `{text, response, model_id, ttl?}` | `{cache_id, exact_key?}` | 写入语义缓存（L0 键 = SHA-256(`text\|model_id`)） |
| `cache_get` | `{text, model_id, threshold?}` | `{hit, score, cache_id?, response?}` | 查询缓存（L0 精确 → L1 语义） |
| `cache_del` | `{cache_id}` | `{deleted}` | 删除缓存条目 |
| `cache_stats` | `{}` | `{entries, hits, misses, hit_rate, evictions, bytes}` | 缓存统计 |
| `ledger_append` | `{session_id, entries:[{entry_type, text?, token_in?, token_out?, source?, ref_id?}]}` | `{ledger_id, appended}` | 追加台账条目（append-only） |
| `ledger_window` | `{session_id}` | `{entries[], total_tokens, warn}` | 会话窗口（active 条目 + 预算告警） |
| `ledger_budget` | `{session_id}` | `{used, limit, headroom}` | 会话预算查询 |
| `ledger_mark` | `{session_id, entry_ids[], status}` | `{updated}` | 追加状态变更记录，原条目不改 |
| `ledger_history` | `{session_id, limit?}` | `{events[]}` | 会话历史全量回放 |
| `ledger_stats` | `{}` | `{sessions, entries, total_tokens}` | 台账统计 |
| `compress` | `{session_id, entries:[{entry_id, entry_type, text}]}` | `{context, saved_tokens, actions[], marked}` | 生成压缩计划并重组上下文，联动 `ledger_mark` / `ledger_append` |
| `health_check` | `{}` | `{service, healthy, record_count, timestamp}` | 服务健康检查 |
| `get_stats` | `{}` | `{daemon, records, max_records}` | 服务统计 |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`mem.write`），由 `gateway_d` 剥离前缀后转发。

## 配置

`--manager <config>` 指定 JSON 配置文件（`daemon` 段）：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/mem.sock",
    "tcp_port": 8085,
    "max_clients": 64,
    "max_records": 1024
  }
}
```

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_MEM_MAX_RECORDS` | 1024 | 记录表上限（取值 `< 65536` 生效） |
| `AIRY_MEM_TFIDF_WEIGHT` | 0.6 | TF-IDF 与 embedding 相似度的融合权重（`0.0`–`1.0`） |
| `AIRY_MEM_EMBEDDING_URL` | 未设置（关闭） | embedding 后端基础 URL（需 libcurl） |
| `AIRY_MEM_EMBEDDING_KEY` | 未设置 | embedding 请求的 Bearer 密钥 |
| `AIRY_MEM_EMB_RETRY_SECONDS` | 60 | embedding 后端故障后的重试间隔 |
| `AIRY_MEM_SOCK` | — | 网关侧覆盖 mem_d 端点 |
| `AIRY_DATA_DIR` | POSIX `/var/lib/agentrt` | 持久化文件根目录 |

## 用法

```bash
airymaxrt logs mem_d           # 运行态日志
<build-dir>/bin/mem_d          # 手工启动单个进程（监听运行目录）
```

直连验证（POSIX）：

```bash
printf '{"jsonrpc":"2.0","id":1,"method":"health_check","params":{}}' \
  | socat - UNIX-CONNECT:"${AIRY_HOME:-$HOME/.airymaxrt}/run/mem.sock"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target mem_d
ctest --test-dir ../daemons-build -R mem_d_ --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

| CTest 用例 | 覆盖点 |
|-----------|--------|
| `mem_d_test_service` | 创建 / 销毁、读写删、容量上限与槽位复用、TF-IDF 排序、embedding 降级、持久化一致性、KB 往返与 UTF-8 安全分块 |
| `mem_d_test_cache` | 缓存确定性、L0 精确命中、L1 阈值边界、TTL 过期、LRU 与字节容量淘汰、模型隔离、命中率统计 |
| `mem_d_test_ledger` | 追加 / 窗口 / 预算、append-only 状态流转可回放、token 计数一致性、参数校验 |
| `mem_d_test_compress` | 压缩确定性、保护规则、超长 tool_result 截断、精确去重、超轮数丢弃、L2 抽取与预算收敛、台账联动回放 |

测试以临时 `AIRY_HOME` / `AIRY_RUNTIME_DIR` 隔离持久化文件。

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | daemon 样板、事件驱动、JSON-RPC dispatcher、生命周期状态机 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、cJSON、token 计数器 |
| [atoms](https://atomgit.com/openairymax/atoms) | 系统调用入口、corekern 运行时初始化 |
| [cupolas](https://atomgit.com/openairymax/cupolas) | 安全穹顶（经 `svc_common` 传递） |
| [gateway_d](../gateway_d/README.md) | 上游：`mem.*` 命名空间转发方 |
| libcurl | 可选，embedding 后端（`AIRY_HAS_CURL`） |
| libyaml | 可选，配置文件解析 |

## 关系

- `mem_d` 是唯一持有记忆状态的进程；其它守护进程经 `gateway_d` 或直接 JSON-RPC 访问。
- 与 `agent_d` 分工：`agent_d` 管 Agent 生命周期，`mem_d` 管 Agent 产生的记忆内容。
- 缓存命中率经 `cache_stats` 暴露，供 `monit_d` 聚合为运行态指标。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
