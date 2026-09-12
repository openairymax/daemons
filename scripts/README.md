# scripts — 构建与质量保障脚本

> **模块路径**: `agentrt/daemons/scripts/`

[![version](https://img.shields.io/badge/version-0.1.15-blue)](https://atomgit.com/openairymax/daemons)
[![license](https://img.shields.io/badge/license-AGPL--3.0--or--later%20OR%20Apache--2.0-green)](../LICENSE)

## 这是什么

`scripts/` 是 `agentrt/daemons` 模块的四个 Bash 脚本，把「构建 → 测试 → 静态分析 →
覆盖率」这条质量流程封装成可直接执行的命令，既给流水线用，也给本地开发自查用。

脚本面向 Linux / macOS 环境（`cppcheck` 固定以 `--platform=unix64` 运行），构建产物一律
落在源码树之外的独立构建目录，不污染源码区。

## 脚本一览

| 脚本 | 作用 | 报告输出位置 |
|------|------|--------------|
| [`ci.sh`](ci.sh) | 一体化流水线入口：依赖检查 → 配置 → 构建 → 测试 → cppcheck → 覆盖率 → 报告 | 外部构建目录下的 `reports/` |
| [`local-ci.sh`](local-ci.sh) | 本地逐模块构建与验证，可按模块单独开关覆盖率 | 外部构建目录下的 `reports/` |
| [`static-analysis.sh`](static-analysis.sh) | 独立的 cppcheck 静态分析与报告生成 | 模块内 `reports/` |
| [`verify-coverage.sh`](verify-coverage.sh) | 独立收集/合并覆盖率数据并与目标值比较 | 外部构建目录下的 `reports/coverage/` |

## ci.sh

```bash
bash scripts/ci.sh [all|build|test|cppcheck|coverage|clean]   # 默认 all
```

- **构建配置**：以 `agentrt/daemons` 为源目录，`CMAKE_BUILD_TYPE=Debug`，
  编译参数 `--coverage -fprofile-arcs -ftest-coverage`、链接参数 `--coverage`，
  并行度取 CPU 核心数。
- **测试**：先 `ctest --output-on-failure -T Test`，再遍历构建目录下的 `test_*`
  可执行文件逐个运行并统计通过/失败数。
- **静态分析**：`--enable=all --std=c11 --platform=unix64 --check-level=exhaustive
  --xml --xml-version=2 --suppress=missingIncludeSystem --suppress=unusedFunction
  --error-exitcode=1`，输出 `reports/cppcheck_report.xml`。
- **覆盖率**：`lcov --capture` → 过滤 `/usr/*` → `genhtml` 生成
  `reports/coverage/` → 用 `lcov --summary` 的行覆盖率与 **80%** 目标比较。
- **构建报告**：`reports/build_report.txt`，汇总各模块归档产物是否生成、覆盖率报告
  是否产出、cppcheck 错误数。
- **依赖**：必需 `cmake` 与 `gcc`/`clang` 之一；`cppcheck`、`gcov`+`lcov`、`genhtml`、
  `bc` 为可选，缺失时对应阶段自动跳过并告警。

## local-ci.sh

```bash
bash scripts/local-ci.sh [build|all|analysis|coverage|report|clean|help]   # 默认 build
```

按 `commons → llm_d → tool_d → monit_d → sched_d → market_d` 的顺序，对每个模块单独
执行 `cmake`（`-DBUILD_TESTS=ON`）+ `make -j` + `ctest`，模块构建目录彼此隔离。

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `BUILD_TYPE` | `Release` | 构建类型 |
| `PARALLEL_JOBS` | CPU 核心数 | 并行作业数 |
| `ENABLE_COVERAGE` | `OFF` | `ON` 时追加 `-DBUILD_COVERAGE=ON` 与 `--coverage` 编译/链接参数 |

- `all` 会强制打开覆盖率，随后依次执行构建、静态分析、覆盖率与报告。
- 静态分析的 cppcheck 参数与 `ci.sh` 相同（不含 `--error-exitcode`），并统计 `<error>` 条数。
- 覆盖率目标同样是 **80%**（硬编码），仅解析 `lines` 行覆盖率。
- **依赖**：必需 `cmake` 与 `gcc`；`cppcheck`、`gcov`+`lcov` 缺失时跳过相应阶段。

## static-analysis.sh

```bash
bash scripts/static-analysis.sh [analyze|html|all|help]   # 默认 all
```

- 分析对象：`commons` 与 `llm_d` / `tool_d` / `market_d` / `monit_d` / `sched_d` 的
  `src/` 目录，附带各自的 `-I <module>/include`，并定义
  `AGENTRT_PLATFORM_LINUX=1`、`AGENTRT_PLATFORM_WINDOWS=0`、`AGENTRT_PLATFORM_MACOS=0`。
- 参数：`--enable=all --std=c11 --platform=unix64 --check-level=exhaustive --inline-suppr
  --xml --xml-version=2 --suppress=missingIncludeSystem --suppress=unusedFunction`。
  若模块根目录存在 `cppcheck.xml`，会作为 `--project` 配置一并载入。
- 输出：`reports/cppcheck_report.xml`（原始结果）、`reports/cppcheck_report.txt`
  （按 error / warning / style / performance / portability / information 分类计数，
  附前 100 条明细）、`reports/cppcheck_html/`（需 `cppcheck-htmlreport`）。
- **依赖**：必需 `cppcheck`（缺失直接报错退出）；`cppcheck-htmlreport` 可选。
  存在 error 级别问题时以非零码退出，便于流水线拦截。

## verify-coverage.sh

```bash
COVERAGE_TARGET=80 bash scripts/verify-coverage.sh
```

1. 检查 `lcov` 与 `genhtml`（缺失即退出）。
2. 遍历外部构建目录下各模块，凡存在 `*.gcda` 即 `lcov --capture` 采集，再合并为
   `reports/coverage/total_coverage.info`；无任何数据时报错并提示先以覆盖率模式构建。
3. 过滤 `/usr/*` 与 `*/tests/*`，用 `genhtml --legend --show-details` 生成
   `reports/coverage/html/`。
4. 取行覆盖率与 `COVERAGE_TARGET`（默认 `80`）比较：达标输出
   `COVERAGE VALIDATION PASSED`；未达标输出 `COVERAGE VALIDATION WARNING`，
   并用 `lcov --list` 列出覆盖率非 100% 的前 20 个 `.c` 文件，返回非零码。
- **依赖**：必需 `lcov`、`genhtml`；`bc` 参与数值比较，缺失时按未达标处理。

## 典型用法

```bash
# 本地一次跑完：逐模块构建 + 静态分析 + 覆盖率 + 报告
bash scripts/local-ci.sh all

# 只做静态分析
bash scripts/static-analysis.sh all

# 只做覆盖率验证，并把目标提高到 90%
COVERAGE_TARGET=90 bash scripts/verify-coverage.sh
```

## 关系

- 构建与测试的开关定义在 [`../CMakeLists.txt`](../CMakeLists.txt)：`BUILD_TESTS`
  默认 `ON`（Windows 下强制关闭）、`BUILD_COVERAGE` 默认 `OFF`。
- 各被测模块的说明见 [守护进程总览](../README_zh.md) 与各自目录下的 README。
- 运行时的部署、启动与日志查看由独立的启动器 CLI 负责，不在本目录范围内。

## 许可

AGPL-3.0-or-later OR Apache-2.0，详见 [LICENSE](../LICENSE)。

Copyright (c) 2025-2026 SPHARX Ltd.
