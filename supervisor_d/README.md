# supervisor_d — daemon 集群监管者

> **模块路径**：`agentrt/daemons/supervisor_d/` · **可执行文件 / CMake 目标**：`supervisor_d` · **RPC 命名空间**：`supervisor.*`

[![Version](https://img.shields.io/badge/version-0.1.18-5a6b7e)](https://atomgit.com/openairymax/daemons)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](../LICENSE)

## 这是什么

`supervisor_d` 是 AirymaxRT daemon 集群的唯一监管者（0.1.18 §12.13 B13）：以声明驱动
方式维持 daemon 集群的期望态——**期望态**是画像 `launch` 声明（`$AIRY_HOME/config/
profile.env` 的 `AIRYRT_LAUNCH_CORE` / `AIRYRT_LAUNCH_AUX` / `AIRYRT_LAUNCH_ARGS_<NAME>`，
launcher 写入、supervisor 只读），**实际态**是子进程集 + 服务端点可达性；每周期比对并
拉齐。从此 launcher 不再直接拉起任何 daemon，唯一启动路径是 `nohup supervisor_d &`。

- **声明驱动调谐（V13.1）**：CORE 集（`AIRYRT_LAUNCH_CORE`）进程死自动复活——waitpid
  收割死因（`exit=N` / `signal=N` 写日志与 `last_death`），按 `base × 2^n` 指数退避重启，
  连败超限转 `failed` 显式告警并停止自动重启（防风暴），`activate` 是复位唯一入口。
- **按需启动（V13.2）**：AUX 集（`AIRYRT_LAUNCH_AUX`）不随 supervisor 启动，仅经
  `supervisor.activate` 单向请求拉起（幂等：已在运行即 no-op）。
- **收摊归一（V13.4）**：`supervisor.shutdown` 统一收割——先 TERM 留 5s 宽限，超时 KILL，
  全部收割后清 pid 文件与 UDS 残留退出。
- **防孤儿**：Linux 子进程 `PR_SET_PDEATHSIG(SIGTERM)`（supervisor 死则子进程收到
  TERM）；Windows Job Object `KILL_ON_JOB_CLOSE`（supervisor 死则整组回收）。
- **谁监管 supervisor_d（⑤）**：不设二级监管者。supervisor 自持
  `<runtime-dir>/supervisor.pid` 防重复启动；异常退出后由
  `_sock_alive supervisor.sock` 探测发现并幂等补拉。

## 架构

```
profile.env launch 声明（期望态 SSoT，launcher 写入）
        │ src/decl/ sup_decl_load（两遍扫描：先名单后 args；缺失回落内置兜底表）
        ▼
main.c 主循环 ── 每 tick ──▶ src/proc/ sup_reconcile
        │                       │ reap（waitpid/WaitForSingleObject 收割死因）
        │                       │ CORE: STOPPED/BACKOFF→spawn（退避门控+超限 failed）
        │                       │ AUX: 仅 activate 拉起
        │                       └ sup_health_tick（sock 可达性假死判定，机制委托 src/net/）
        └ src/ctrl/ sup_ctrl_serve（控制口 JSON-RPC，串行单连接）
```

域切分（gen5 五件套，codegen=false 手写装配）：装配对 `src/main.c`（壳）+
`src/svc.c`（常驻生命周期）· `src/decl/` 期望态解析 · `src/proc/` 进程域
（spawn/reap/退避/假死探测/收摊）· `src/ctrl/` 控制口 · `src/net/` 端点机制件
（跨平台 sock 可达性探测 + Windows TCP 装配权威）。域依赖单向
`decl`/`proc`/`ctrl`/`net` → 契约，全户共享契约与 net 对外接口经
`src/supervisor_d_internal.h` 单点消费。

**V13.5 硬约束**：零项目内库链接（`link-whitelist.txt` 允许集为空，linkgate 构建期
fail-closed 检验）。UDS/TCP、进程原语、JSON 字段提取、日志全部自持，仅链接系统库
（POSIX 纯 libc；Windows `ws2_32`）——保证监管者自身不随任何业务库劣化而失效。

## 控制口协议

端点：POSIX Unix socket `<runtime-dir>/supervisor.sock`（权限 0600）；Windows 本机
TCP 回环 `127.0.0.1:8095`；`AIRY_SUPERVISOR_SOCK` 可覆盖。单行 JSON 请求，串行处理。

| 方法 | 请求 | 行为 |
|------|------|------|
| `supervisor.activate` | `{"name":"monit_d"}` | 拉起指定 daemon（幂等；FAILED 复位唯一入口） |
| `supervisor.shutdown` | `{}` | 收摊归一，收割全部子进程后退出 |
| `health_check` | `{}` | 返回全量进程表状态摘要 |

命令行客户端（连接已运行实例）：

```sh
supervisor_d                 # 常驻监管（唯一启动路径：nohup supervisor_d &）
supervisor_d activate monit_d  # 按需拉起 AUX
supervisor_d status            # health_check 摘要
supervisor_d stop              # 收摊
```

## 期望态声明（profile.env）

```sh
AIRY_PROFILE=full
AIRYRT_LAUNCH_CORE="gateway_d llm_d think_d agent_d tool_d"
AIRYRT_LAUNCH_AUX="monit_d sched_d channel_d market_d cupolas_d mem_d a2a_d notify_d"
AIRYRT_LAUNCH_ARGS_llm_d="--manager /path/model.yaml"
AIRYRT_LAUNCH_KEEPENV_llama_server="LD_LIBRARY_PATH"
```

- 名单先于 args 应用（两遍扫描）；`AIRYRT_LAUNCH_ARGS_<NAME>` 对未登记名 fail-closed 拒绝。
- 外部进程托管：`AIRYRT_LAUNCH_BIN_<NAME>` 覆盖可执行路径，`AIRYRT_LAUNCH_EP_<NAME>`
  声明探活通道（`tcp:<port>` 回环或 UDS 路径），三键同样对未登记名 fail-closed 拒绝。
- 环境隔离（D5）：POSIX 子进程派生前一律清除 `LD_LIBRARY_PATH` / `LD_PRELOAD` /
  `LD_AUDIT`，仅 `AIRYRT_LAUNCH_KEEPENV_<NAME>` 显式列名者保留——宿主 shell 或上层
  包装进程的注入不再改变受管 daemon 的加载器搜索路径与预载库。Windows 腿无此三变量，
  `KEEPENV` 仍被解析但派生时不参与。
- 声明缺失时回落内置缺省表（5 CORE + 9 AUX），仅升级兼容兜底，不构成第二套清单。
- 端点解析与 gateway 一致：`AIRY_<NS>_SOCK` env 覆盖 → POSIX `<runtime-dir>/<ns>.sock` →
  Windows 固定 TCP 表。maths 因与 a2a 的 Windows 端口冲突（B17 订正中）不入表，其
  Windows 腿仅进程存活判定。

## 调参环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `AIRYRT_SUP_TICK_MS` | 5000 | 调谐周期 |
| `AIRYRT_SUP_BACKOFF_BASE_MS` | 1000 | 退避基值（`base × 2^n`） |
| `AIRYRT_SUP_BACKOFF_MAX_MS` | 30000 | 退避上限 |
| `AIRYRT_SUP_MAX_ATTEMPTS` | 5 | 连败超限阈值（转 failed 告警） |
| `AIRYRT_SUP_LIVENESS_TICKS` | 3 | 端点不可达假死判定周期数 |
