# think_d — 双思考系统守护进程

> 命名空间：`think.*`
> Unix socket：`${AIRY_RUNTIME_DIR}/think.sock`（Windows：`\\.\pipe\airy_think`）
> TCP 端口：8090（`--tcp` 启用，默认仅 Unix socket）
> 请求缓冲上限：1 MiB（`MAX_BUFFER`）

## 定位

`think_d` 承载 AgentRT 的**双思考系统（Thinkdual）认知引擎**（CoreLoopThree），
将提示词转换为行动计划与思考事件 JSON，是认知层的核心驱动组件。

双思考模型（GRAD 批判循环）：

- **t2 慢思考**（`think2_slow_model`，模型 A）：规划器 + Phase-2 生成（GRAD s2）；
- **t1-f 快思考**（`think1_fast_model`，模型 B）：上下文仲裁——GRAD s1 最终
  接受/拒绝判定，日常对话路由；
- **t1-p 专业思考**（`think1_prof_model`，模型 C）：逻辑校验——GRAD 中确定性
  四查（zero-token）门，非 LLM 仲裁。

对话请求先经 GCCP 事实锁（Phase 0 意图确认）+ GRAD 逻辑锁（Phase-1 计划级
批判循环）产出收敛计划，再进入执行；`enabled: false` 时退化为单轮普通处理。
工作记忆键：`gccp_goal`、`cog_review_decision`。

LLM 调用经 `llm_svc_adapter` 直连 `llm_d` Unix socket（`daemon_rpc_call`），
本地端点（Ollama/vLLM 等）与云端 API（deepseek/openai/anthropic/google/glm/
qwen/moonshot/siliconflow/spark/custom）走同一 socket，由 llm_d 路由做
策略级回退（成本感知 → 轮询链）。

关键能力：

- **GCCP 两段式交互（P-A）**：当引擎判定输入需要澄清时——第一段
  `gccp_answers` 传 NULL/空串，引擎挂起并返回问题集
  （`gccp_need_interaction:1` + `gccp_questions`，返回码 0），不浪费后续
  Phase token；第二段携带答案 JSON（如 `{"endpoint":"...","start":"..."}`）
  重发，引擎完成目标确认并走完 GCCP+GRAD 全链路；答案单次有效（第二段后
  服务端清空，第三段不带答案重新进入第一段语义）。用户放弃问答可直接不重发。
- **会话隔离**：GCCP 交互状态（问题集/答案）按 `session_id` 维度隔离，
  杜绝多客户端并发串台（`session_id` 为 NULL/空时使用 `"default"` 兜底）。
- **流程编排（S-5）**：`think.orchestrate` 走 orchestrator 管线
  （分解→规划→生成→批判→验证→审计→对齐），含熔断/重试/超时/进度回调，
  与 `think.process`（单次认知引擎）双管线并存。

## 架构

```
CLI/TUI → gateway_d ──(think.process / think.orchestrate)──▶ think_d
      │                                                       ├─ CoreLoopThree 认知引擎
      │                                                       │   ├─ GCCP 事实锁（Phase 0 意图确认）
      │                                                       │   ├─ GRAD 批判循环（t2 规划 → t1-p 校验 → t1-f 终裁）
      │                                                       │   └─ 产出 plan/feedback/stats JSON
      │                                                       └─ llm_svc_adapter ──(llm.complete)──▶ llm_d
```

- 服务层 `src/think_service.c` 抽为静态库 `airy_think_service`，核心依赖
  `airy_coreloopthree`（其 PUBLIC 传递 airy_cognition/airy_core/airy_memory/
  airy_taskflow/svc_common 及 coreloopthree 头文件）。
- `think_d` 实现编排接口并注入 ops 表（`think_orch_ops_inject`），atoms 侧组件
  （如工作大厅）可经 `are_ops_get_orch()` 调度编排，无需链接 daemons。

## JSON-RPC 接口

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `think.process` | `{prompt: string, gccp_answers?: string(JSON), session_id?: string}` | `result`：**JSON 字符串** | 双思考处理。正常完成含 `plan`（task_plan_id/node_count/nodes）、`feedback`（level/module/event/data 数组）、`stats`（dual_thinking_enabled/dual_invocations/corrections）；问答轮含 `gccp_need_interaction:1` 与 `gccp_questions`（JSON 字符串数组） |
| `think.orchestrate` | `{input: string, timeout_ms?: int}` | `{run_id, phases: [{phase, status, error_code, duration_ms, output}], success}` | 流程编排执行（orchestrator 管线 7 阶段）；管线中途失败返回 0 且 `success:false` |
| `think.get_stats` | `{}` | 统计 JSON | 引擎健康与调用统计（dual_invocations/corrections/llm_backed 等） |
| `think.health_check` | `{}` | `{service, healthy, timestamp}` | 服务健康检查（引擎与适配器就绪） |
| `think.shutdown` | `{}` | — | 优雅退出 |

## 配置

配置来源优先级：**环境变量 > `$AIRY_CONFIG_DIR/model.yaml` 的 `think:` 段 >
`-c/--manager` JSON 配置 > 默认值**（模型角色名经 `svc_model_defaults`
公共层从 model.yaml 读取，与 gateway_d 读全局段同一模式）。

`model.yaml`（`$AIRY_CONFIG_DIR` 下）：

```yaml
think:
  enabled: true
  think2_slow_model: ""      # 慢思考（t2）生成行动计划（GRAD 模型 A）
  think1_fast_model: ""      # 快思考（t1-f）对计划终裁（GRAD 模型 B）
  think1_prof_model: ""      # 专业思考（t1-p）专家校验（GRAD 模型 C）
  timeout_ms: 120000
```

`--manager <config>` JSON 配置（`daemon` 段 + `think` 段，遗留兼容）：

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/think.sock",
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

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRY_THINK_ENABLED` | 1 | 双思考总开关（0/false/no 关闭） |
| `AIRY_THINK2_SLOW_MODEL` | 空 | t2 慢思考模型角色 |
| `AIRY_THINK1_FAST_MODEL` | 空 | t1-f 快思考模型角色 |
| `AIRY_THINK1_PROF_MODEL` | 空 | t1-p 专业思考模型角色 |
| `AIRY_THINK_TIMEOUT_MS` | 120000 | process 超时（毫秒） |

## 依赖与构建

依赖：`airy_coreloopthree`（CoreLoopThree 认知引擎）、`airy_cognition`、
`airy_llm_service`、`airy_tool_service`、`svc_common`、`airy_common`、cJSON。
GNU ld 下 coreloopthree ↔ cognition ↔ llm_service/tool_service 存在静态库循环
依赖，链接使用 `-Wl,--start-group/--end-group`（与 gateway_d 同款方案）。

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target think_d
```

安装：`cmake --install` 将 `think_d` 装入 `bin`，头文件装入 `include/agentrt`。

## 测试

```bash
ctest --test-dir build -R think_ --output-on-failure
```

集成测试（`tests/test_think_gccp_twopass.c` → `test_think_gccp_twopass`，不依赖
llm_d：LLM 不可用时 GCCP probe 走启发式降级、`need_interaction` 恒为 1，
链路可确定性验证）覆盖：

- **两段式闭环**：第一段（无答案）返回 0 且含 `gccp_need_interaction:1` 与
  非空 `gccp_questions`（不含 plan，挂起）；第二段（携带 `gccp_answers`）返回
  0 且含 `plan`，feedback 中 `intent_confirmed` 事件标记 `interacted:1`；
- **答案单次有效**：第二段后服务端暂存答案被清理——第三次调用（不带答案）
  重新进入第一段语义，答案不泄漏到下一轮。
