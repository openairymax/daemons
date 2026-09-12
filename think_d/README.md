# think_d — 双思考认知引擎守护进程

> **模块路径**：`agentrt/daemons/think_d/` · **可执行文件 / CMake 目标**：`think_d` · **RPC 命名空间**：`think.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`think_d` 承载 AgentRT 的**双思考系统（ThinkDual）认知引擎**（CoreLoopThree）：
把一条提示词转换为行动计划与思考事件流，同时对外提供推理语言网关与执行复核两类
语义判断能力。它是认知层的唯一长驻服务，LLM 调用一律经 `llm_svc_adapter`
转发到 `llm_d`。

- 端点：POSIX Unix socket `<runtime-dir>/think.sock`（`$AIRY_HOME/run/think.sock`）；
  Windows 固定为本机 TCP 回环 `127.0.0.1:8090`。
- 可选 TCP：POSIX 上以 `--tcp` 启用，默认端口 `8090`（默认只监听 socket）。
- 请求缓冲上限 1 MiB；事件循环 `max_events` 16，工作线程池 2~4、队列 32。

双思考角色（GRAD 批判循环）：

| 角色 | 配置项 | 职责 |
|------|--------|------|
| **t2 慢思考**（模型 A） | `think2_slow_model` | 规划与生成，产出行动计划 |
| **t1-f 快思考**（模型 B） | `think1_fast_model` | 上下文仲裁，对计划做最终接受 / 拒绝判定 |
| **t1-p 专业思考**（模型 C） | `think1_prof_model` | 逻辑校验，确定性四查门（不消耗 token） |

请求先经 GCCP 事实锁（意图确认）与 GRAD 批判循环收敛出计划，再进入执行；
`enabled: false` 时退化为单轮普通处理。

## 能力

- **双思考处理** — 提示词 → 行动计划 + 思考事件流 + 统计，结果以 JSON 字符串返回。
- **GCCP 两段式问答** — 引擎判定输入需要澄清时挂起并返回问题集，客户端携带答案
  重发即完成确认，避免在未收敛的意图上消耗后续 token。答案单次有效。
- **会话隔离** — 问答状态按 `session_id` 维度隔离，多客户端并发不串台；
  缺省或空串归入 `default` 会话。
- **流程编排** — `orchestrate` 走 orchestrator 管线（分解 → 规划 → 生成 → 批判 →
  验证 → 审计 → 对齐），与 `process`（单次认知引擎调用）双管线并存。
- **推理语言网关** — `lang_process` 做输入标准化，`lang_postprocess` 做输出后处理，
  `lang_stats` 暴露网关统计。网关句柄懒创建，LLM 不可用时降级为启发式判定。
- **执行复核** — `review` 对执行产物做 t2 语义偏离判定与 t1-f 接受 / 拒绝终裁；
  对应角色模型未配置时返回 `verdict: -1`，由调用方走既定降级，避免同模型自审自签。

## 架构

```
CLI / SDK ──▶ gateway_d ──(think.process / orchestrate / lang_* / review)──▶ think_d
                                                                           │
                   llm_d ◀──(llm_svc_adapter → llm.complete)───────────────┤
                                                                           ├─ CoreLoopThree 引擎
                                                                           │   ├─ GCCP 事实锁（意图确认）
                                                                           │   ├─ GRAD 批判循环（t2 → t1-p → t1-f）
                                                                           │   └─ plan / feedback / stats
                                                                           ├─ lang_gateway 服务面
                                                                           └─ orchestrator 管线
```

- 服务层 `src/think_service.c` + `src/think_orch.c` 抽为静态库 `airy_think_service`，
  被守护进程可执行文件与单元测试共用；依赖 `airy_coreloopthree`（其 PUBLIC 传递
  `airy_cognition` / `airy_core` / `airy_memory` / `airy_taskflow` / `svc_common`）。
- `src/lang_svc.c`、`src/review_svc.c` 只注册 RPC 服务面，机制本体留在
  `atoms/coreloopthree`，不复制到本模块。
- 引擎内的 IPC / LLM / tool 调用经 ops 表注入（`daemon_ipc_ops_init` /
  `daemon_llm_ops_init` / `daemon_tool_ops_init`），认知引擎不直接链接 daemons 符号；
  注入失败非致命，调用点自行降级。
- 事件驱动并发模式开启（`concurrent_clients = true`）：`process` 含分钟级 LLM
  思考，同步模式会阻塞事件循环导致其他连接排队、健康检查误报掉线。

## JSON-RPC 接口

共 9 个方法，经 `method_dispatcher_register` 注册：

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `process` | `{prompt: string, gccp_answers?: string(JSON), session_id?: string}` | **JSON 字符串** | 双思考处理。完成时含 `plan`（`task_plan_id` / `node_count` / `nodes[]` / `entry_points`）、`feedback`（`level` / `module` / `event` / `data` 数组）、`stats`；问答轮含 `gccp_need_interaction: 1` 与 `gccp_questions`（不含 `plan`） |
| `orchestrate` | `{input: string, timeout_ms?: int}` | `{run_id, phases: [{phase, status, error_code, duration_ms, output}], success}` | 编排管线执行；`phase` 取 `decomposition`/`planning`/`generation`/`critique`/`verification`/`audit`/`alignment`，`status` 取 `pending`/`running`/`completed`/`failed`/`cancelled`/`timeout`；任一阶段未 `completed` 则 `success: false` |
| `get_stats` | `{}` | 引擎健康 JSON + `{dual_invocations, dual_corrections, llm_adapter_connected}` | 认知引擎健康与双思考统计 |
| `health_check` | `{}` | `{service: "think_d", healthy, timestamp}` | 服务健康检查（引擎与 LLM 适配器就绪） |
| `lang_process` | `{text: string, model_id?: string, history_tokens?: int}` | `{raw_input, system_prompt, transformed_input, reasoning_lang, output_lang, decision_reason, decision_chain, telemetry_json}` | 推理语言网关输入标准化 |
| `lang_postprocess` | `{text: string, expected_lang?: int}` | `{text}` | 模型输出后处理 |
| `lang_stats` | `{}` | 网关统计 JSON | 语言网关可观测性数据 |
| `review` | `{stage: "t2" \| "t1f", node_goal?, output_json?, output_signatures?: array (t2), gate_reason?: string (t1f), drift?: int (t1f)}` | `{verdict: -1 \| 0 \| 1, reason}` | 执行复核。`t2`：`1` 偏离 / `0` 满足；`t1f`：`1` 拒绝 / `0` 接受；`-1` 表示角色模型未配置或判定不可用，调用方走降级。未知 `stage` 返回 `-32602` |
| `shutdown` | `{}` | — | 优雅退出 |

外部调用方使用带命名空间前缀的形式（`think.process`），由 `gateway_d` 剥离前缀后转发。

`process` 的内部失败优先返回引擎自带的诊断 JSON（`stats.err_code` 等），仅在 JSON
缺失时才回退通用 `-32603`，客户端可据此区分真实失败与可降级路径。

## 配置

生效优先级：**环境变量 > `$AIRY_CONFIG_DIR/model.yaml` 的 `think:` 段 >
`--manager <config>` JSON > 内建默认值**。

`model.yaml`（位于 `$AIRY_CONFIG_DIR`，与 `gateway_d` 读取全局段同一公共层）：

```yaml
think:
  enabled: true
  think2_slow_model: ""      # t2 慢思考：生成行动计划
  think1_fast_model: ""      # t1-f 快思考：计划终裁
  think1_prof_model: ""      # t1-p 专业思考：确定性逻辑校验
  timeout_ms: 120000
```

`--manager <config>` 为 JSON 文件，`daemon` 段决定端点，`think` 段为上表同名字段：

```json
{
  "daemon": {
    "socket_path": "<runtime-dir>/think.sock",
    "tcp_port": 8090
  },
  "think": {
    "enabled": true,
    "think2_slow_model": "",
    "think1_fast_model": "",
    "think1_prof_model": "",
    "timeout_ms": 120000
  }
}
```

配置文件中出现 `daemon.tcp_port` 会同时启用 TCP 监听。

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_HOME` | `~/.airymaxrt` | 运行时目录根，socket 位于 `$AIRY_HOME/run/think.sock` |
| `AIRY_CONFIG_DIR` | — | `model.yaml` 所在目录 |
| `AIRY_THINK_ENABLED` | 1 | 双思考总开关（`0` / `false` / `no` 关闭） |
| `AIRY_THINK2_SLOW_MODEL` | 空 | t2 慢思考模型角色 |
| `AIRY_THINK1_FAST_MODEL` | 空 | t1-f 快思考模型角色 |
| `AIRY_THINK1_PROF_MODEL` | 空 | t1-p 专业思考模型角色 |
| `AIRY_THINK_TIMEOUT_MS` | 120000 | `process` 超时（毫秒） |
| `AIRY_THINK_SOCK` | — | 网关侧覆盖 think_d 端点 |

## 用法

```bash
airymaxrt logs think_d          # 运行态日志
<build-dir>/bin/think_d         # 手工启动单个进程（监听默认端点）
```

直连端点排查（绕过 `gateway_d`，方法名不带前缀）：

```bash
printf '%s' '{"jsonrpc":"2.0","id":1,"method":"health_check","params":{}}' |
  socat - UNIX-CONNECT:"${AIRY_HOME:-$HOME/.airymaxrt}/run/think.sock"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target think_d
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

```bash
ctest --test-dir ../daemons-build -R think_d_ --output-on-failure
```

| ctest 名称 | 覆盖内容 |
|------------|----------|
| `think_d_test_think_gccp_twopass` | GCCP 两段式问答闭环 |

该用例不依赖 `llm_d`：LLM 不可用时 GCCP 探测走启发式降级、`need_interaction`
恒为 1，链路可确定性验证。覆盖两段式闭环（无答案 → 挂起并返回非空问题集；
带答案 → 返回 `plan` 且 `intent_confirmed` 事件标记 `interacted: 1`）与答案
单次有效（第三段不带答案重新回到第一段语义，答案不泄漏到下一轮）。

## 依赖

| 依赖 | 用途 |
|------|------|
| [atoms](https://atomgit.com/openairymax/atoms) | `airy_coreloopthree`（认知引擎、GCCP、GRAD、lang_gateway、orchestrator）、`airy_cognition` |
| [llm_d](../llm_d/README.md) | 所有 LLM 调用的下游，含策略级回退路由 |
| [tool_d](../tool_d/README.md) | 引擎内工具调用 ops 表的落地端 |
| [`svc_common`](../common/README.md) | 守护进程样板、事件驱动、JSON-RPC dispatcher、`svc_model_defaults` |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、日志、内存与同步原语、cJSON 封装 |

静态库之间存在 `coreloopthree ↔ cognition ↔ llm_service / tool_service` 循环引用，
GNU ld 下以 `-Wl,--start-group/--end-group` 链接（与 `gateway_d` 同款方案）；
Windows 目标另链 `ws2_32`、`bcrypt`。

## 关系

`think_d` 只做认知决策，不做模型路由与工具执行：文本生成一律下发 `llm_d`，
工具调用下发 `tool_d`，计划落地为 DAG 后由 `sched_d` 派发。执行链路需要
语义判定（偏离检测、接受 / 拒绝终裁）时调用 `think.review`，判定策略集中在
本模块，不在调用方进程内复制。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
