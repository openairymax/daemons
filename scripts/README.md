# scripts — CI/CD 与质量保障脚本

> **模块路径**: `agentrt/daemons/scripts/`

## 定位

`scripts/` 提供 AgentRT 守护进程模块的 CI/CD 与质量保障脚本：构建、测试、静态
分析与代码覆盖率验证，供 CI 流水线与本地开发复用。

## 目录结构

```
scripts/
├── README.md                # 本文件
├── ci.sh                    # CI 流水线入口（全流程）
├── local-ci.sh              # 本地 CI 模拟（逐模块构建）
├── static-analysis.sh       # cppcheck 静态分析
└── verify-coverage.sh       # 覆盖率收集与目标验证
```

## ci.sh — CI 流水线入口

| 子命令 | 说明 |
|--------|------|
| `all` | 完整流程（默认）：依赖检查 → 构建 → 测试 → cppcheck → 覆盖率 → 报告 |
| `build` | 依赖检查 + 配置 + 构建 |
| `test` | 仅运行测试（ctest + 直接运行 `build/test_*` 可执行文件） |
| `cppcheck` | 仅静态分析 |
| `coverage` | 仅覆盖率分析 |
| `clean` | 清理构建目录（`${PROJECT_ROOT}/../AgentRT-build`） |

- CMake Debug 构建，启用 `--coverage -fprofile-arcs -ftest-coverage`；
- cppcheck：`--enable=all --std=c11 --platform=unix64 --check-level=exhaustive
  --error-exitcode=1`，报告 `reports/cppcheck_report.xml`；
- 覆盖率目标 80%（`lcov --summary` 提取行覆盖率，`bc -l` 比较）；
- **必需依赖**：`cmake`、`gcc` 或 `clang`、`ctest`（测试路径）；
  **可选依赖**：`cppcheck`、`gcov`/`lcov`、`genhtml`、`bc`（覆盖率路径）。

## local-ci.sh — 本地 CI 模拟

| 子命令 | 说明 |
|--------|------|
| `build` | 构建所有模块（默认） |
| `all` | 构建 + 静态分析 + 覆盖率 + 报告（强制 `ENABLE_COVERAGE=ON`） |
| `analysis` | 仅静态分析 |
| `coverage` | 仅覆盖率分析（需先构建） |
| `report` | 生成构建报告 |
| `clean` | 清理构建目录 |
| `help` | 帮助 |

- 构建模块顺序：`commons → llm_d → tool_d → monit_d → sched_d → market_d`，
  各模块独立 `cmake + make` 并运行 ctest；
- 报告输出：`${PROJECT_ROOT}/../AgentRT-build/reports/`（`build_report.txt`、
  `cppcheck_report.xml`、`coverage/`）；
- 环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TYPE` | `Release` | 构建类型 |
| `PARALLEL_JOBS` | CPU 核心数 | 并行作业数 |
| `ENABLE_COVERAGE` | `OFF` | 启用覆盖率编译（`ON` 时加 `-DBUILD_COVERAGE=ON --coverage`） |

- **必需依赖**：`cmake`、`gcc`；**可选依赖**：`cppcheck`（缺失跳过静态分析）、
  `gcov`/`lcov`（缺失跳过覆盖率）、`bc`（覆盖率比较，带 `|| echo "0"` 回退）。

## static-analysis.sh — cppcheck 静态分析

| 子命令 | 说明 |
|--------|------|
| `analyze` | 分析 + 摘要 |
| `html` | 仅生成 HTML 报告 |
| `all` | 完整流程（默认）：分析 → HTML → 摘要 |
| `help` | 帮助 |

- 分析目录：`agentrt/commons/src`、`llm_d/src`、`tool_d/src`、`market_d/src`、
  `monit_d/src`、`sched_d/src`（含对应 `-I include` 与平台宏定义）；
- 配置：`--enable=all --std=c11 --platform=unix64 --check-level=exhaustive
  --inline-suppr`，抑制 `missingIncludeSystem` / `unusedFunction`；
- 输出：`reports/cppcheck_report.xml`（XML）、`reports/cppcheck_report.txt`
  （按 error/warning/style/performance/portability/information 统计）、
  `reports/cppcheck_html/`（HTML，可选）；
- **必需依赖**：`cppcheck`（缺失即报错退出）；**可选依赖**：
  `cppcheck-htmlreport`（HTML 报告）。

## verify-coverage.sh — 覆盖率验证

- 流程：工具检查 → 逐模块收集 gcda → 合并 → 过滤（`/usr/*`、`*/tests/*`）→
  HTML 报告 → 行覆盖率对比目标；
- 数据源：`${PROJECT_ROOT}/../AgentRT-build/daemons/{commons,llm_d,tool_d,monit_d,
  sched_d,market_d}` 下的 `*.gcda`（需先以覆盖率模式构建，如
  `ENABLE_COVERAGE=ON bash scripts/local-ci.sh build`）；
- 输出：`reports/coverage/total_coverage.info`、`reports/coverage/html/`；
- 结果：达标输出 `COVERAGE VALIDATION PASSED`；未达标输出
  `COVERAGE VALIDATION WARNING` 并列出未充分覆盖文件；
- 环境变量：`COVERAGE_TARGET`（默认 `80`）；
- **必需依赖**：`lcov`、`genhtml`（缺失即报错退出）；`bc` 参与比较（带
  `|| echo "0"` 回退，缺失时视为未达标）。

## 使用示例

```bash
# CI 全流程
bash scripts/ci.sh all

# 本地逐模块构建（Release）
bash scripts/local-ci.sh build

# 本地全流程（含覆盖率与分析）
ENABLE_COVERAGE=ON bash scripts/local-ci.sh all

# 静态分析
bash scripts/static-analysis.sh all

# 覆盖率验证（自定义目标 90%）
COVERAGE_TARGET=90 bash scripts/verify-coverage.sh
```

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
