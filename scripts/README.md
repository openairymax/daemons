# scripts — 构建与质量保障脚本

> **模块路径**: `agentrt/daemons/scripts/`

[![version](https://img.shields.io/badge/version-0.1.19-blue)](https://atomgit.com/openairymax/daemons)
[![license](https://img.shields.io/badge/license-AGPL--3.0--or--later%20OR%20Apache--2.0-green)](../LICENSE)

## 这是什么

`scripts/` 是 `agentrt/daemons` 模块的四个 Bash 脚本，把「构建 → 测试 → 静态分析 →
覆盖率」这条质量流程封装成可直接执行的命令，既给流水线用，也给本地开发自查用。

脚本面向 Linux / macOS 环境（`cppcheck` 固定以 `--platform=unix64` 运行）。构建与报告
产物一律落在源码树之外的独立构建目录（默认 `~/.cache/agentrt/daemons`，可用环境变量
`AIRYRT_BUILD_ROOT` 覆盖），不污染源码区。

所有脚本遵循 **fail-closed** 原则：测试失败、cppcheck 发现问题、覆盖率未达标都以非零
码退出，杜绝吞错放行；模块清单动态发现（遍历 `daemons/*/CMakeLists.txt`），新增守护
服务无需修改脚本。

## 脚本一览

| 脚本 | 作用 | 报告输出位置 |
|------|------|--------------|
| [`ci.sh`](ci.sh) | 一体化流水线入口：依赖检查 → 配置 → 构建 → 测试 → cppcheck → 覆盖率 → 报告 | 构建目录下的 `reports/` |
| [`local-ci.sh`](local-ci.sh) | 本地单树构建与验证，可按需开关覆盖率 | 构建目录下的 `reports/` |
| [`static-analysis.sh`](static-analysis.sh) | 独立的 cppcheck 静态分析与报告生成 | 构建目录下的 `reports/` |
| [`verify-coverage.sh`](verify-coverage.sh) | 独立收集/合并覆盖率数据并与目标值比较 | 构建目录下的 `reports/coverage/` |

## ci.sh

```bash
bash scripts/ci.sh [all|build|test|cppcheck|coverage|clean]   # 默认 all
```

- **构建配置**：以 `agentrt` 根为源目录配置单一构建树（`daemons/CMakeLists.txt` 依赖
  根工程提供的变量，不能独立配置），`CMAKE_BUILD_TYPE=Debug`，编译参数
  `--coverage -fprofile-arcs -ftest-coverage`、链接参数 `--coverage`，并行度取 CPU
  核心数。
- **测试**：单树内 `ctest --output-on-failure` 统一执行全部注册测试，任一失败即整
  体失败。
- **静态分析**：`--enable=all --std=c11 --platform=unix64 --check-level=exhaustive
  --xml --xml-version=2 --suppress=missingIncludeSystem --suppress=unusedFunction
  --error-exitcode=1`，分析对象为 `daemons/` 源码，输出 `reports/cppcheck_report.xml`；
  发现问题即以非零码退出。
- **覆盖率**：`lcov --capture` → 过滤 `/usr/*` → `genhtml` 生成
  `reports/coverage/` → 用 `lcov --summary` 的行覆盖率与 **80%** 目标比较，未达标
  以非零码退出。
- **构建报告**：`reports/build_report.txt`，汇总各模块构建目录是否生成、覆盖率报告
  是否产出、cppcheck 错误数。
- **依赖**：必需 `cmake`、`gcc`/`clang` 之一、`ctest`；`cppcheck`、`gcov`+`lcov`、
  `genhtml` 为可选，缺失时对应阶段跳过并告警。数值比较使用 `awk`（不再依赖 `bc`）。

## local-ci.sh

```bash
bash scripts/local-ci.sh [build|all|analysis|coverage|report|clean|help]   # 默认 build
```

单一构建树模型：`cmake` 以 `agentrt` 根配置一次，`cmake --build` 全量构建，
`ctest --output-on-failure` 执行全部测试（fail-closed）。模块清单动态发现。

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `BUILD_TYPE` | `Release` | 构建类型 |
| `PARALLEL_JOBS` | CPU 核心数 | 并行作业数 |
| `ENABLE_COVERAGE` | `OFF` | `ON` 时追加 `--coverage` 编译/链接参数 |
| `AIRYRT_BUILD_ROOT` | `~/.cache/agentrt/daemons` | 构建目录（源码区之外） |

- `all` 会强制打开覆盖率，随后依次执行构建测试、静态分析、覆盖率与报告。
- 静态分析对象为各模块 `src/`，附带 commons 与各模块 `include/`，启用
  `--error-exitcode=1`，发现问题即失败。
- 覆盖率按模块在 `daemons/<module>/` 构建子目录采集，合并后与 **80%** 目标比较，
  未达标即失败。
- **依赖**：必需 `cmake`、`gcc`、`ctest`；`cppcheck`、`gcov`+`lcov` 缺失时跳过相应
  阶段。

## static-analysis.sh

```bash
bash scripts/static-analysis.sh [analyze|html|all|help]   # 默认 all
```

- 分析对象与包含目录动态发现：各模块 `src/` 与 `include/`，外加
  `agentrt/commons/include`，并定义 `AGENTRT_PLATFORM_LINUX=1`、
  `AGENTRT_PLATFORM_WINDOWS=0`、`AGENTRT_PLATFORM_MACOS=0`。
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
2. 动态发现模块，凡构建子目录存在 `*.gcda` 即 `lcov --capture` 采集，再合并为
   `reports/coverage/total_coverage.info`；无任何数据时报错退出并提示先以覆盖率
   模式构建。
3. 过滤 `/usr/*` 与 `*/tests/*`，用 `genhtml --legend --show-details` 生成
   `reports/coverage/html/`。
4. 取行覆盖率与 `COVERAGE_TARGET`（默认 `80`）比较：达标输出
   `COVERAGE VALIDATION PASSED`；未达标输出 `COVERAGE VALIDATION WARNING`，
   并用 `lcov --list` 列出覆盖率非 100% 的前 20 个 `.c` 文件，返回非零码。
- **依赖**：必需 `lcov`、`genhtml`。数值比较使用 `awk`。

## 典型用法

```bash
# 本地一次跑完：单树构建 + 测试 + 静态分析 + 覆盖率 + 报告
bash scripts/local-ci.sh all

# 只做静态分析
bash scripts/static-analysis.sh all

# 只做覆盖率验证，并把目标提高到 90%
COVERAGE_TARGET=90 bash scripts/verify-coverage.sh

# 自定义构建目录
AIRYRT_BUILD_ROOT=/path/to/build bash scripts/ci.sh all
```

## 关系

- 构建与测试的开关定义在 [`../CMakeLists.txt`](../CMakeLists.txt)：`BUILD_TESTS`
  默认 `ON`（Windows 下强制关闭）、`BUILD_COVERAGE` 默认 `OFF`。
- 各被测模块的说明见 [守护进程总览](../README_zh.md) 与各自目录下的 README。
- 运行时的部署、启动与日志查看由独立的启动器 CLI 负责，不在本目录范围内。

## 许可

AGPL-3.0-or-later OR Apache-2.0，详见 [LICENSE](../LICENSE)。

Copyright (c) 2025-2026 SPHARX Ltd.
