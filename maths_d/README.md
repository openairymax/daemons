# maths_d — 数学外挂计算服务

> **命名空间**：`maths.*`
> **Unix socket**：`${AIRY_RUNTIME_DIR}/maths.sock`（默认 `/tmp/agentrt/maths.sock`）
> **TCP 端口**：8087（Windows 回退）

## 定位

`maths_d` 是 AgentRT 运行时的**数学外挂计算器**（用户态 daemon）。把数学计算从 LLM
推理中剥离：一个 `125*38/7.2+15` 的表达式由 LLM 推理需 150~300 token 且易出错；
经本服务求值只返回一个数值（约 5 token），节省 90%+ Token 且结果确定。

按"数学计算建议稿"六级分类，双引擎覆盖：

| 引擎 | 能力 | 实现 |
|------|------|------|
| **纯 C 快速路径** | 基础算术、初等函数、描述性统计、模式识别 | `src/expr_eval.c`（递归下降）+ `src/maths_service.c`，零外部依赖、毫秒级 |
| **Python 符号后端** | 方程求解、微积分、极限、化简/展开/因式分解、矩阵、单位换算、金融、数论 | `src/python_backend.c` → stdio JSON-RPC → maths-toolkit（MCP-Mathematics + sympy-mcp，共享 `$AIRY_HOME/venv`） |

后端不可用时自动降级到纯 C 路径（`get_stats` 中 `python_backend` 字段标记
`up/degraded`），不阻塞 agentrt 核心。

## 架构

```
┌────────────────────────────────────────────────────────────┐
│  gateway_d / SDK                                            │
│     │  JSON-RPC 2.0 over Unix socket（maths.* 命名空间）     │
│     ▼                                                       │
│  maths_d（main.c）                                          │
│   ├─ maths_service.c   JSON-RPC 分发（eval/stats/recognize/ │
│   │                    符号方法转发，手写 JSON 解析）        │
│   ├─ expr_eval.c       纯 C 递归下降求值器（独立模块）       │
│   └─ python_backend.c  stdio JSON-RPC 子进程管理（常驻 worker│
│                        + EPIPE 限次重启 + 降级）             │
│     │                                                       │
│     ▼                                                       │
│  maths-toolkit（ecosystem/markets/tools/maths-toolkit）      │
│   └─ $AIRY_HOME/backend/maths_backend.py                    │
│      ├─ MCP-Mathematics：numerical / units / finance /       │
│      │                  number_theory（AST 安全求值）        │
│      └─ SymPy：solve / differentiate / integrate / limit /   │
│                 simplify / factor / expand / matrix          │
└────────────────────────────────────────────────────────────┘
```

## JSON-RPC 接口

### 本地方法（纯 C 快速路径）

| 方法 | 参数 | 返回 |
|------|------|------|
| `eval` | `{"expr":"125*38/7.2+15"}` | `{"expr", "result", "elapsed_ms"}` |
| `stats` | `{"op":"mean","values":[1,2,3,4,5]}` | `{"op","count","result"}`（op: sum/mean/avg/max/min/median/variance/var/stddev） |
| `recognize` | `{"text":"sqrt(144)"}` | `{"is_math":1}` |
| `health_check` | — | `{"status","service","eval_count","error_count","last_eval_ms"}` |
| `get_stats` | — | `{"service","eval_count","stats_count","symbolic_count","python_backend","error_count","last_eval_ms","uptime_sec"}` |
| `shutdown` | — | `{"status":"shutting_down"}` |

### 符号/数值后端方法（委托 maths-toolkit，需 Python 后端就绪）

| 方法 | 功能 | 示例参数 |
|------|------|---------|
| `solve` | 方程/方程组求解 | `{"equation":"x**2-4=0","symbol":"x"}` |
| `differentiate` | 求导/偏导 | `{"expr":"x**3+2*x","symbol":"x","order":1}` |
| `integrate` | 定/不定积分 | `{"expr":"x**2","symbol":"x","a":"0","b":"1"}` |
| `limit` | 极限 | `{"expr":"sin(x)/x","symbol":"x","to":"0"}` |
| `simplify` / `factor` / `expand` | 化简/因式分解/展开 | `{"expr":"(x**2-1)/(x-1)"}` |
| `matrix` | 矩阵（det/inv/transpose/multiply/eigen） | `{"op":"det","a":[[1,2],[3,4]]}` |
| `numerical` | 通用数值求值（MCP-Mathematics 52 函数） | `{"expr":"sin(pi/6)+sqrt(144)"}` |
| `units` | 单位换算（158 种，15 类量纲） | `{"value":1.0,"from":"km","to":"m"}` |
| `finance` | 金融（百分比/税率/利息/贷款/折扣/加价） | `{"op":"percentage","value":200,"percentage":10}` |
| `number_theory` | 数论（素数判定/质因数分解） | `{"op":"is_prime","n":17}` |

`eval` 本地支持的函数：`sqrt sin cos tan asin acos atan atan2 exp ln log
log10 log2 abs floor ceil round sign sinh cosh tanh cbrt pow min max mod
remainder factorial`；常量 `pi`、`e`。

## 安全边界

- 纯 C 路径仅数值求值：无代码执行、无文件系统访问、无网络请求；
- 表达式字符集白名单预检（防注入），长度 ≤ 4096、解析深度 ≤ 64；
- 除零/取模零/阶乘越界/NaN/Inf 显式报错；
- Python 路径数值求值走 MCP-Mathematics AST 安全求值（白名单操作符/函数、
  超时与内存上限）；符号输入由 SymPy 受限解析（sympify）；
- 仅监听 Unix Socket（Windows 回退本机 TCP）。

## 配置

无配置文件解析（main 忽略 argc/argv）。行为常量：

| 常量 | 值 | 说明 |
|------|-----|------|
| `MATHS_MAX_EXPR_LEN` | 4096 | 表达式最大长度 |
| `MATHS_MAX_DEPTH` | 64 | 解析递归深度 |
| `MATHS_MAX_VALUES` | 65536 | 统计数组上限 |
| `MATHS_DEFAULT_PORT` | 8087 | Windows TCP 回退端口 |

## 接线

- 构建：顶层 `daemons/CMakeLists.txt` 已 `add_subdirectory(maths_d)`；
- 启动：`agentrt-bootstrap.sh` Layer 2（`llm_d think_d ... maths_d`）；
- 出厂预装：agentrt 主 `install.sh` 默认调用 maths-toolkit 安装器
  （`--without-maths` 跳过，失败降级纯 C 路径）；
- 消费：gateway 在工具循环中对数学表达式优先路由 `maths.sock`（`eval`），
  失败回退 LLM 推理（不阻断主流程）。

## 测试

```bash
ctest -R maths_service --output-on-failure
```

用例：基础算术、初等函数、错误路径、统计、模式识别、后端方法路由
（12 个后端方法在 backend 不可用时返回 backend 错误而非 method not found）。
