# maths_d — 数学外挂计算服务

`maths_d` 是 AgentRT 运行时的**数学外挂计算器**（用户态 daemon，`maths.*` 命名空间）。

## 定位

把数学计算从 LLM 推理中剥离：一个 `125*38/7.2+15` 的表达式由 LLM 推理
需 150~300 token，且易出错；经本服务求值只返回一个数值（约 5 token）。
纯本地 C 实现，零外部依赖、零网络请求、零代码执行。

## 能力范围

| 层级 | 能力 | 示例 |
|------|------|------|
| 基础算术 | 四则/幂/取模/阶乘/绝对值 | `125*38/7.2+15`、`2^10`、`5!` |
| 初等函数 | 三角/对数/指数/双曲/取整 | `sin(pi/6)`、`log(8,2)`、`sqrt(144)` |
| 统计 | 均值/中位数/方差/标准差/和/极值 | `mean([1,2,3,4,5])` |
| 模式识别 | 判断文本是否应路由到数学外挂 | `recognize("sqrt(144)")` |

符号计算（微积分/ODE/矩阵特征值等）不在本服务范围，由上层路由到
外部可插拔的符号计算服务。

## RPC 接口（Unix Socket `maths.sock`，JSON-RPC 2.0）

| 方法 | 参数 | 返回 |
|------|------|------|
| `eval` | `{"expr":"125*38/7.2+15"}` | `{"result":674.611111111111}` |
| `stats` | `{"op":"mean","values":[1,2,3,4,5]}` | `{"op":"mean","count":5,"result":3}` |
| `recognize` | `{"text":"sqrt(144)"}` | `{"is_math":1}` |
| `health_check` | — | `{"status":"healthy",...}` |
| `get_stats` | — | `{"eval_count":N,...}` |
| `shutdown` | — | `{"status":"shutting_down"}` |

`eval` 支持的函数：`sqrt sin cos tan asin acos atan atan2 exp ln log log10
log2 abs floor ceil round sign sinh cosh tanh cbrt pow min max mod
remainder factorial`；常量 `pi`、`e`。

## 安全边界

- 只做数值求值：无代码执行、无文件系统访问、无网络请求；
- 表达式字符集白名单预检（防注入），长度 ≤ 4096、解析深度 ≤ 64；
- 除零/取模零/阶乘越界/NaN/Inf 显式报错；
- 仅监听 Unix Socket（`maths.sock`），Windows 回退本机 127.0.0.1 TCP。

## 接线

- 构建：顶层 `daemons/CMakeLists.txt` 已 `add_subdirectory(maths_d)`；
- 启动：`agentrt-bootstrap.sh` Layer 2（`llm_d think_d ... maths_d`）；
- 消费：gateway 在工具循环中对数学表达式优先路由 `maths.sock`（`eval`），
  失败回退 LLM 推理（不阻断主流程）。

## 测试

```bash
ctest -R maths_service --output-on-failure
```
