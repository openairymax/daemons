#!/bin/bash
# Copyright (c) 2026 SPHARX. All Rights Reserved.
# SPDX-FileCopyrightText: 2026 SPHARX Ltd.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
# AgentRT daemon 本地 CI/CD 验证脚本
# 单一构建树验证：配置 → 构建 → ctest → 静态分析 → 覆盖率

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKS_ROOT="$(dirname "$SCRIPT_DIR")"
AGENTRT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
PARALLEL_JOBS="${PARALLEL_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
ENABLE_COVERAGE="${ENABLE_COVERAGE:-OFF}"
# 构建目录必须在源码区之外（铁律）；可用 AIRYRT_BUILD_ROOT 覆盖
BUILD_DIR="${AIRYRT_BUILD_ROOT:-${HOME}/.cache/agentrt/daemons}"
REPORT_DIR="${BUILD_DIR}/reports"

# 动态发现 daemons 模块（含 common 公共库目录）
DAEMON_MODULES=()
for _d in "${BACKS_ROOT}"/*/CMakeLists.txt; do
    [ -e "$_d" ] || continue
    DAEMON_MODULES+=("$(basename "$(dirname "$_d")")")
done

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

echo "=========================================="
echo "  AgentRT daemon Local CI/CD Validator"
echo "=========================================="
echo ""
log_info "Source Root: $AGENTRT_ROOT"
log_info "Build Dir:   $BUILD_DIR"
log_info "Build Type:  $BUILD_TYPE"
log_info "Parallel Jobs: $PARALLEL_JOBS"
log_info "Coverage: $ENABLE_COVERAGE"
echo ""

if [ ${#DAEMON_MODULES[@]} -eq 0 ]; then
    log_error "未发现任何 daemons 模块"
    exit 1
fi
log_info "发现模块: ${DAEMON_MODULES[*]}"
echo ""

# 检查依赖
check_dependencies() {
    log_info "检查构建依赖..."

    local missing_deps=0

    for cmd in cmake gcc ctest; do
        if ! command -v "$cmd" &> /dev/null; then
            log_error "缺少依赖: $cmd"
            missing_deps=1
        fi
    done

    # 检查可选依赖
    CPPCHECK_AVAILABLE=false
    if command -v cppcheck &> /dev/null; then
        CPPCHECK_AVAILABLE=true
        log_info "cppcheck 已安装"
    else
        log_warn "cppcheck 未安装，将跳过静态分析"
    fi

    COVERAGE_AVAILABLE=false
    if command -v gcov &> /dev/null && command -v lcov &> /dev/null; then
        COVERAGE_AVAILABLE=true
        log_info "gcov/lcov 已安装"
    else
        log_warn "gcov/lcov 未安装，将跳过代码覆盖率"
    fi

    if [ $missing_deps -eq 1 ]; then
        log_error "缺少必要依赖，请先安装"
        exit 1
    fi

    log_success "依赖检查通过"
}

# 清理构建目录
clean() {
    log_info "清理构建目录..."
    case "${BUILD_DIR}" in
        ""|"/") log_error "非法构建目录: ${BUILD_DIR}"; exit 1 ;;
    esac
    rm -rf "${BUILD_DIR}"
    log_success "清理完成"
}

# 配置单一构建树
configure_tree() {
    log_info "配置单一构建树..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    local cmake_args="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"

    if [ "$ENABLE_COVERAGE" = "ON" ] && [ "$COVERAGE_AVAILABLE" = true ]; then
        cmake_args="$cmake_args -DCMAKE_C_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage"
    fi

    cmake "${AGENTRT_ROOT}" $cmake_args
    log_success "构建树配置完成"
}

# 构建并测试
build_all() {
    configure_tree
    log_info "构建（-j$PARALLEL_JOBS）..."
    cmake --build . -j"$PARALLEL_JOBS"

    log_info "运行全部测试（fail-closed）..."
    ctest --output-on-failure -j"$PARALLEL_JOBS"

    log_success "构建与测试完成"
}

# 代码静态分析
static_analysis() {
    if [ "$CPPCHECK_AVAILABLE" = false ]; then
        log_warn "cppcheck 未安装，跳过静态分析"
        return 0
    fi

    log_info "运行静态分析..."

    mkdir -p "$REPORT_DIR"
    local CPPCHECK_REPORT="$REPORT_DIR/cppcheck_report.xml"

    local CPPCHECK_ARGS="--enable=all --std=c11 --platform=unix64"
    CPPCHECK_ARGS="$CPPCHECK_ARGS --xml --xml-version=2"
    CPPCHECK_ARGS="$CPPCHECK_ARGS --suppress=missingIncludeSystem"
    CPPCHECK_ARGS="$CPPCHECK_ARGS --suppress=unusedFunction"
    CPPCHECK_ARGS="$CPPCHECK_ARGS -j$PARALLEL_JOBS"
    CPPCHECK_ARGS="$CPPCHECK_ARGS --error-exitcode=1"

    # 包含目录：commons（agentrt 根）+ 各模块 include
    CPPCHECK_ARGS="$CPPCHECK_ARGS -I ${AGENTRT_ROOT}/commons/include"
    local module
    for module in "${DAEMON_MODULES[@]}"; do
        if [ -d "${BACKS_ROOT}/${module}/include" ]; then
            CPPCHECK_ARGS="$CPPCHECK_ARGS -I ${BACKS_ROOT}/${module}/include"
        fi
    done

    # 分析对象：各模块 src/
    local analysis_dirs=()
    for module in "${DAEMON_MODULES[@]}"; do
        if [ -d "${BACKS_ROOT}/${module}/src" ]; then
            analysis_dirs+=("${BACKS_ROOT}/${module}/src")
        fi
    done

    if [ ${#analysis_dirs[@]} -eq 0 ]; then
        log_error "未找到任何模块源码目录"
        return 1
    fi

    cppcheck $CPPCHECK_ARGS \
        --output-file="$CPPCHECK_REPORT" \
        "${analysis_dirs[@]}" \
        2>/dev/null

    log_success "静态分析完成"
}

# 代码覆盖率分析
coverage_analysis() {
    if [ "$COVERAGE_AVAILABLE" = false ]; then
        log_warn "gcov/lcov 未安装，跳过覆盖率分析"
        return 0
    fi

    if [ "$ENABLE_COVERAGE" != "ON" ]; then
        log_warn "未启用覆盖率编译，跳过覆盖率分析"
        return 0
    fi

    log_info "运行代码覆盖率分析..."

    local COVERAGE_DIR="$REPORT_DIR/coverage"
    mkdir -p "$COVERAGE_DIR"

    # 按模块采集覆盖率数据（单树布局：BUILD_DIR/daemons/<module>/）
    local all_info_files=""
    local module module_build
    for module in "${DAEMON_MODULES[@]}"; do
        module_build="${BUILD_DIR}/daemons/${module}"
        if [ -d "$module_build" ]; then
            cd "$module_build"
            if ls *.gcda 1>/dev/null 2>&1; then
                lcov --capture --directory . --output-file "${module}_coverage.info"
                all_info_files="$all_info_files -a ${module}_coverage.info"
            fi
        fi
    done

    if [ -z "$all_info_files" ]; then
        log_error "未找到任何覆盖率数据"
        return 1
    fi

    cd "$BUILD_DIR"
    lcov $all_info_files -o "$COVERAGE_DIR/total_coverage.info"

    # 过滤系统头文件
    lcov --remove "$COVERAGE_DIR/total_coverage.info" '/usr/*' --output-file "$COVERAGE_DIR/total_coverage.info"

    # 生成HTML报告
    genhtml "$COVERAGE_DIR/total_coverage.info" --output-directory "$COVERAGE_DIR/html"

    # 提取覆盖率百分比
    local coverage_output coverage_percent
    coverage_output=$(lcov --summary "$COVERAGE_DIR/total_coverage.info" 2>&1)
    coverage_percent=$(echo "$coverage_output" | grep -oP 'lines.*: \K[\d.]+(?=%)' | head -1)

    if [ -n "$coverage_percent" ]; then
        log_info "代码覆盖率: ${coverage_percent}%"

        # 检查是否达到80%（fail-closed：未达标即失败）
        if awk -v v="$coverage_percent" 'BEGIN { exit !(v >= 80.0) }'; then
            log_success "代码覆盖率达标 (>= 80%)"
        else
            log_error "代码覆盖率未达标 (< 80%): ${coverage_percent}%"
            return 1
        fi
    fi

    log_success "覆盖率报告已生成: $COVERAGE_DIR/html/index.html"
}

# 生成构建报告
generate_report() {
    log_info "生成构建报告..."

    local REPORT_FILE="$REPORT_DIR/build_report.txt"

    {
        echo "========================================"
        echo "AgentRT daemon 构建报告"
        echo "========================================"
        echo ""
        echo "构建时间: $(date)"
        echo "构建类型: $BUILD_TYPE"
        echo ""
        echo "--- 模块状态 ---"
        local module
        for module in "${DAEMON_MODULES[@]}"; do
            if [ -d "${BUILD_DIR}/daemons/${module}" ]; then
                echo "  ${module}: ✓ 已构建"
            else
                echo "  ${module}: ✗ 未构建"
            fi
        done
        echo ""
        echo "--- 测试状态 ---"
        if [ -d "$REPORT_DIR/coverage" ]; then
            echo "  覆盖率报告: ✓ 已生成"
        else
            echo "  覆盖率报告: ✗ 未生成"
        fi
        echo ""
        echo "--- 静态分析 ---"
        if [ -f "$REPORT_DIR/cppcheck_report.xml" ]; then
            local error_count
            error_count=$(grep -c '<error' "$REPORT_DIR/cppcheck_report.xml" 2>/dev/null || echo "0")
            echo "  cppcheck问题数: ${error_count}"
        else
            echo "  cppcheck: 未运行"
        fi
        echo ""
        echo "========================================"
    } > "$REPORT_FILE"

    log_success "构建报告已生成: $REPORT_FILE"
}

# 打印使用说明
usage() {
    echo ""
    echo "用法: $0 [命令]"
    echo ""
    echo "命令:"
    echo "  clean          清理构建目录"
    echo "  build          配置、构建并运行全部测试"
    echo "  all            构建 + 静态分析 + 覆盖率 + 报告（强制开启覆盖率）"
    echo "  analysis       仅运行静态分析"
    echo "  coverage       仅运行覆盖率分析（需先构建）"
    echo "  report         生成构建报告"
    echo "  help           显示此帮助信息"
    echo ""
    echo "环境变量:"
    echo "  BUILD_TYPE          构建类型 (Release/Debug)，默认为 Release"
    echo "  PARALLEL_JOBS       并行作业数，默认为 CPU 核心数"
    echo "  ENABLE_COVERAGE     启用覆盖率 (ON/OFF)，默认为 OFF"
    echo "  AIRYRT_BUILD_ROOT   构建目录，默认为 \$HOME/.cache/agentrt/daemons"
    echo ""
}

# 主逻辑
case "${1:-build}" in
    clean)
        clean
        ;;
    build)
        check_dependencies
        build_all
        log_success "构建与测试完成!"
        ;;
    all)
        ENABLE_COVERAGE=ON
        check_dependencies
        build_all
        static_analysis
        coverage_analysis
        generate_report
        log_success "全部完成!"
        ;;
    analysis)
        static_analysis
        ;;
    coverage)
        coverage_analysis
        ;;
    report)
        generate_report
        ;;
    help|--help|-h)
        usage
        ;;
    *)
        log_error "未知命令: $1"
        usage
        exit 1
        ;;
esac

echo ""
log_success "脚本执行完成"
