# think_d — 双思考系统守护进程（think.* 命名空间）

> **命名空间**：`think.*`
> **Unix socket**：`${AIRY_RUNTIME_DIR}/think.sock`
> **配置来源**：`$AIRY_HOME/config/model.yaml` 的 `think:` 段（模型角色表）

## 职责

`think_d` 承载 AgentRT 的**双思考系统（Thinkdual / GRAD 批判循环）**，是
认知层的核心驱动组件，也是未来"端云混合策略"（云端大模型组织逻辑制定任务
图纸、本地小模型执行任务）的技术底座。

- 慢思考（t2）生成行动计划（GRAD 模型 A）；
- 快思考（t1-f）对计划终裁（GRAD 模型 B）；
- 专业思考（t1-p）专家校验（GRAD 模型 C）。

对话请求先经批判循环（GRAD）产出收敛的 DAG 行动计划，再交 LLM 按计划回答；
`enabled: false` 时退化为单轮普通计划。

## JSON-RPC 方法

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `think.process` | `{prompt, session?}` | `{plan, thinking_event}` | 双思考处理：提示词 → 行动计划 + 思考事件 JSON |
| `think.orchestrate` | `{prompt, config?}` | `{result}` | 流程编排执行（S-5 orchestrator 管线，7 阶段） |
| `think.get_stats` | `{}` | `{engine_health, call_count}` | 双思考引擎健康与调用统计 |
| `think.health_check` | `{}` | `{status}` | 服务健康检查 |

## 模型角色配置

三个思考角色模型在 `model.yaml` 的 `think:` 段配置，值须为 `models` 表中某行
的 `model_id`；留空 `""` 表示使用 `default_model`（三个角色共用）。

```yaml
think:
  enabled: true
  think2_slow_model: ""      # 慢思考（t2）生成行动计划（GRAD 模型 A）
  think1_fast_model: ""      # 快思考（t1-f）对计划终裁（GRAD 模型 B）
  think1_prof_model: ""      # 专业思考（t1-p）专家校验（GRAD 模型 C）
  timeout_ms: 120000
```

临时覆盖（env 优先级最高）：`AIRY_THINK_ENABLED / AIRY_THINK2_SLOW_MODEL /
AIRY_THINK1_FAST_MODEL / AIRY_THINK1_PROF_MODEL / AIRY_THINK_TIMEOUT_MS`。

优先级：env > model.yaml `think:` 段 > think_d `-c` JSON 配置 > 默认。

## 架构

```
CLI/TUI → gateway_d ──(think.process)──▶ think_d ──(llm.complete)──▶ llm_d
                                              │
                                              ├─ GRAD 批判循环（t2 规划 / t1-f 终裁 / t1-p 校验）
                                              └─ 产出 DAG 行动计划 → 执行层调度
```

## 配置

```json
{
  "daemon": {
    "socket_path": "/tmp/agentrt/think.sock",
    "max_clients": 64
  }
}
```

## 构建与测试

```bash
cmake --build ${AIRY_HOME}/build --target think_d
ctest --test-dir ${AIRY_HOME}/build -R cl3_grad --output-on-failure
```
