# 测试债台账（daemons）

对应 0.1.21 施工项 **L-12**，口径与 [`../TEST-DEBT.md`](../TEST-DEBT.md) 一致：
头文件中以**函数原型**声明的 `airy_*` 导出符号，必须在测试树中被引用至少一次。

daemons 是「策略/装配层」：进程入口经 `daemon_main.h` 的服务循环宏展开，
请求处理体由各 daemon 提供；对外可见的 `airy_*` 均来自链入的机制库
（commons/atoms），本仓头文件本身不导出新 API。

## 1. 当期实测（2026-11-17）

```bash
python3 tools/scripts/ci/quality/src/test_debt.py \
    --root agent-workload/agentrt/daemons
```

| 项 | 值 |
|---|---:|
| 导出原型 | 0 |
| 被测试引用 | 0 |
| 缺口 | 0 |
| 引用率 | 0.0%（无导出面，分母为 0） |

## 2. 口径订正留痕

旧台账登记 `6 / 3 / 3 / 50.0%`，三条「缺口」在修正后的正则下均不成立：

| 旧条目 | 实际形态 | 判定 |
|---|---|---|
| `airy_daemon_read_request` | `common/include/daemon_main.h` 服务循环宏体内的调用 | 调用，非原型 |
| `airy_sock_close` | 同上宏体内 `JSONRPC_SEND_ERROR` 失败分支的调用 | 调用，非原型 |
| `airy_token_standard_count` | `llm_d/src/router/core/router_context.h` 的 `static inline` 体内调用 | 私有内联体调用，非导出 |

三者真正的声明位置都不在 daemons 头文件：`airy_daemon_read_request` 与
`airy_sock_close` 声明在 `commons/platform/include/`，`airy_token_standard_count`
声明在 `commons/utils/token/`。它们属于 [`../TEST-DEBT.md`](../TEST-DEBT.md)
的历史缺口（按「并发与 IPC 优先」销账），不构成本仓的导出面；旧台账把调用点
当成声明位置，才误记到 daemons 名下。

## 3. 后续约束

daemons 新增导出原型时，本表分母随之变化，必须同时补测试树引用；
CI 的 `g48-test-debt.sh` 在 `AIRY_COMMIT_BASE..HEAD` 范围内对新增符号自动拦截。
