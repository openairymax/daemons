# 测试债台账（daemons）

对应 0.1.21 施工项 **L-12**，口径与 [`../TEST-DEBT.md`](../TEST-DEBT.md) 一致：
头文件中声明的 `airy_*` 导出符号，必须在测试树中被引用至少一次。

## 1. 当期实测（2026-11-17）

| 项 | 值 |
|---|---:|
| 导出符号 | 6 |
| 被测试引用 | 3 |
| 缺口 | 3 |
| 引用率 | 50.0% |

## 2. 缺口清单

| 符号 | 声明位置 | 影响 |
|---|---|---|
| `airy_daemon_read_request` | `common/include/daemon_main.h` | 请求读取路径无直接用例，帧边界错误只能在集成层暴露 |
| `airy_sock_close` | `common/include/daemon_main.h` | 关闭路径无直接用例，fd 泄漏与重复关闭缺防线 |
| `airy_token_standard_count` | `llm_d/src/router/core/router_context.h` | 标准 token 统计无直接用例，计费与限流口径可能漂移 |

## 3. 销账要求

三条缺口按上表顺序补用例：先 `airy_sock_close`（句柄生命周期风险最高），
再 `airy_daemon_read_request`，最后 `airy_token_standard_count`。
补完后复跑同一脚本，`uncovered` 应降为 0；后续新增符号由 CI 的
`g48-test-debt.sh` 在 `AIRY_COMMIT_BASE..HEAD` 范围内自动拦截。
