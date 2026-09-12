# maths_d — 数学计算守护进程

> **模块路径**：`agentrt/daemons/maths_d/` · **可执行文件 / CMake 目标**：`maths_d` · **RPC 命名空间**：`maths.*`

[![Version](https://img.shields.io/badge/version-0.1.15-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`maths_d` 把数学计算从 LLM 推理中剥离出来，作为一个独立的用户态服务。像
`125*38/7.2+15` 这样的表达式交给 LLM 推理要消耗上百 token 且容易算错，交给本
服务只返回一个确定数值。它采用**双引擎**：纯 C 快速路径覆盖日常算术与统计，
Python 符号后端覆盖方程求解与微积分；后端缺失时自动降级，不影响运行时启动。

- 端点：POSIX Unix socket `<runtime-dir>/maths.sock`（`$AIRY_HOME/run/maths.sock`）；
  Windows 本机 TCP 回环 `127.0.0.1:8087`。
- 符号后端：由 [ecosystem](https://atomgit.com/openairymax/ecosystem) 中的
  `markets/tools/maths-toolkit` 包部署到 `$AIRY_HOME/backend/maths_backend.py`，
  解释器取 `$AIRY_HOME/venv/bin/python3`。

## 能力

| 引擎 | 覆盖范围 | 实现 |
|------|---------|------|
| **纯 C 快速路径** | 四则与幂运算、初等函数、描述性统计、数学表达式识别 | `src/expr_eval.c`（递归下降求值器）+ `src/maths_service.c`，零外部依赖 |
| **Python 符号后端** | 方程求解、求导、积分、极限、化简 / 因式分解 / 展开、矩阵、通用数值、单位换算、金融、数论 | `src/python_backend.c` → stdio JSON-RPC → maths-toolkit（SymPy + MCP-Mathematics） |

- **自动降级** — 后端脚本或解释器不存在时，进程照常启动并只对外暴露本地方法；
  `get_stats` 的 `python_backend` 字段标记 `up` / `degraded`。
- **常驻 worker + 限次重启** — 后端以子进程常驻，通信异常时按次数上限重启，避免崩溃循环。
- **网关优先路由** — `gateway_d` 在工具循环中识别数学表达式并优先调用 `eval`，
  失败则回退 LLM 推理，不阻断主流程。

## 架构

```
gateway_d / SDK
   │  JSON-RPC 2.0 · maths.* 命名空间
   ▼
maths_d ── src/main.c            监听、accept、单请求短连接
        ── src/maths_service.c   方法分发、参数校验、统计计数
        ── src/expr_eval.c       纯 C 递归下降求值器
        └─ src/python_backend.c  fork/pipe 子进程、stdio JSON-RPC、降级
             │
             ▼
        $AIRY_HOME/backend/maths_backend.py
          ├─ SymPy：solve / differentiate / integrate / limit /
          │         simplify / factor / expand / matrix
          └─ MCP-Mathematics：numerical / units / finance / number_theory
```

## JSON-RPC 接口

共 18 个方法，由 `maths_service.c` 按方法名分发（`method` 不带 `maths.` 前缀）。

### 本地方法（纯 C 快速路径，6 个）

| 方法 | 参数 | 返回 | 描述 |
|------|------|------|------|
| `eval` | `{expr}` | `{expr, result, elapsed_ms}` | 表达式求值 |
| `stats` | `{op, values[]}` | `{op, count, result}` | 描述性统计，`op` ∈ `sum` / `mean` / `avg` / `max` / `min` / `median` / `variance` / `var` / `stddev` |
| `recognize` | `{text}` | `{is_math: 0\|1}` | 判断文本是否为数学表达式 |
| `health_check` | `{}` | `{status, service, eval_count, error_count, last_eval_ms}` | 存活探针 |
| `get_stats` | `{}` | `{service, eval_count, stats_count, symbolic_count, python_backend, error_count, last_eval_ms, uptime_sec}` | 运行统计与后端状态 |
| `shutdown` | `{}` | `{status:"shutting_down"}` | 优雅退出 |

`eval` 支持的函数：`sqrt sin cos tan asin acos atan atan2 exp ln log log10 log2
abs floor ceil round sign sinh cosh tanh cbrt pow min max mod remainder factorial`；
支持的常量：`pi`、`e`。

### 后端方法（委托 maths-toolkit，12 个）

后端未就绪时，这 12 个方法返回后端错误而非「方法不存在」。

| 方法 | 功能 | 示例参数 |
|------|------|---------|
| `solve` | 方程 / 方程组求解 | `{"equation":"x**2-4=0","symbol":"x"}` |
| `differentiate` | 求导 / 偏导 | `{"expr":"x**3+2*x","symbol":"x","order":1}` |
| `integrate` | 定积分 / 不定积分 | `{"expr":"x**2","symbol":"x","a":"0","b":"1"}` |
| `limit` | 极限 | `{"expr":"sin(x)/x","symbol":"x","to":"0"}` |
| `simplify` | 化简 | `{"expr":"(x**2-1)/(x-1)"}` |
| `factor` | 因式分解 | `{"expr":"x**2-1"}` |
| `expand` | 展开 | `{"expr":"(x+1)**3"}` |
| `matrix` | 矩阵运算 | `{"op":"det","a":[[1,2],[3,4]]}`，`op` ∈ `det` / `inv` / `transpose` / `multiply` / `eigen` |
| `numerical` | 通用数值求值 | `{"expr":"sin(pi/6)+sqrt(144)"}` |
| `units` | 单位换算 | `{"value":1.0,"from":"km","to":"m"}` |
| `finance` | 金融计算 | `{"op":"percentage","value":200,"percentage":10}` |
| `number_theory` | 数论 | `{"op":"is_prime","n":17}` |

外部调用方使用带命名空间前缀的形式（`maths.eval`），由 `gateway_d` 剥离前缀后转发。

## 安全边界

- 纯 C 路径只做数值求值：不执行代码、不访问文件系统、不发起网络请求；
- 表达式做字符集白名单预检，长度与解析深度分别受 `MATHS_MAX_EXPR_LEN`、
  `MATHS_MAX_DEPTH` 约束；
- 除零、模零、阶乘参数越界、`NaN` / `Inf` 显式报错，不静默返回垃圾值；
- Python 侧的数值求值走 AST 安全求值（白名单操作符与函数、超时与内存上限），
  符号输入由 SymPy 受限解析；
- 只监听本机端点（POSIX Unix socket / Windows 回环 TCP）。

## 配置

`maths_d` **没有配置文件，也不解析命令行参数**（`main()` 不使用 `argc/argv`）。

| 常量 | 值 | 说明 |
|------|-----|------|
| `MATHS_DEFAULT_SOCKET` | `airy_runtime_dir_socket("maths.sock")` | POSIX 端点 |
| `MATHS_DEFAULT_PORT` | 8087 | Windows TCP 回环端口 |
| `MATHS_MAX_EXPR_LEN` | 4096 | 表达式最大长度 |
| `MATHS_MAX_DEPTH` | 64 | 解析递归深度上限 |
| `MATHS_MAX_VALUES` | 65536 | 统计数组长度上限 |

| 环境变量 | 说明 |
|----------|------|
| `AIRY_HOME` | 符号后端定位根目录，依次探测 `$AIRY_HOME/venv/bin/python3` 与 `$AIRY_HOME/backend/maths_backend.py` |
| `AIRY_MATHS_SOCK` | 网关侧覆盖 maths_d 端点 |

后端按需装配：安装 [ecosystem](https://atomgit.com/openairymax/ecosystem) 中的
`markets/tools/maths-toolkit` 包（`install.sh`）即部署脚本与依赖 wheel；不装则
`maths_d` 以纯 C 路径运行。

## 用法

```bash
airymaxrt logs maths_d         # 运行态日志
<build-dir>/bin/maths_d        # 手工启动单个进程（监听运行目录）
```

直连验证（POSIX）：

```bash
printf '{"jsonrpc":"2.0","id":1,"method":"eval","params":{"expr":"125*38/7.2+15"}}' \
  | socat - UNIX-CONNECT:"${AIRY_HOME:-$HOME/.airymaxrt}/run/maths.sock"
```

构建（构建目录须位于源码树之外）：

```bash
cmake -S . -B ../daemons-build -DBUILD_TESTS=ON
cmake --build ../daemons-build --target maths_d
ctest --test-dir ../daemons-build -R test_maths_service --output-on-failure
```

Windows 源码构建默认不编译守护进程，需显式 `-DBUILD_DAEMON=ON`。

## 测试

| CTest 用例 | 覆盖点 |
|-----------|--------|
| `test_maths_service` | 基础算术、初等函数、错误路径、统计、表达式识别、后端方法路由 |

## 依赖

| 依赖 | 用途 |
|------|------|
| [`svc_common`](../common/README.md) | 生命周期状态机、日志、服务注册 |
| [commons](https://atomgit.com/openairymax/commons) | 平台路径、socket / 线程抽象、cJSON |
| [ecosystem](https://atomgit.com/openairymax/ecosystem) | `markets/tools/maths-toolkit` —— Python 符号后端包 |
| [gateway_d](../gateway_d/README.md) | 上游：`maths.*` 命名空间转发方与表达式优先路由 |

## 关系

`maths_d` 与 `tool_d` 属于不同层次：`tool_d` 管理工具清单与调用生命周期，
`maths_d` 是一个具体的确定性计算服务，被网关在数学表达式场景下直接寻址。
它不依赖 `llm_d`，也不反向调用模型——所有推理侧的回退由调用方决定。

## 许可

Copyright (c) 2025-2026 SPHARX Ltd.

双许可证，二选一：**AGPL-3.0-or-later** 或 **Apache-2.0**。
SPDX-License-Identifier: `AGPL-3.0-or-later OR Apache-2.0`。
完整许可文本见 [`../LICENSE`](../LICENSE)，版权与核心 IP 声明见 [`../NOTICE`](../NOTICE)。
