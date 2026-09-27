#!/bin/bash
# Copyright (c) 2026 SPHARX. All Rights Reserved.
# SPDX-FileCopyrightText: 2026 SPHARX Ltd.
# SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
# AgentRT Backs模块CI/CD脚本
# 包含：构建、测试、静态分析、代码覆盖率

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKS_DIR="$(dirname "$SCRIPT_DIR")"
AGENTRT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# 构建目录必须在源码区之外（铁律）；可用 AIRYRT_BUILD_ROOT 覆盖
BUILD_DIR="${AIRYRT_BUILD_ROOT:-${HOME}/.cache/agentrt/daemons}"
REPORT_DIR="${BUILD_DIR}/reports"

# 动态发现 daemons 模块（含 common 公共库目录）
DAEMON_MODULES=()
for _d in "${BACKS_DIR}"/*/CMakeLists.txt; do
    [ -e "$_d" ] || continue
    DAEMON_MODULES+=("$(basename "$(dirname "$_d")")")
done

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."
    
    local missing_deps=()
    
    # 检查cmake
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    # 检查编译器
    if ! command -v gcc &> /dev/null && ! command -v clang &> /dev/null; then
        missing_deps+=("gcc or clang")
    fi
    
    # 检查cppcheck（可选）
    CPPCHECK_AVAILABLE=false
    if command -v cppcheck &> /dev/null; then
        CPPCHECK_AVAILABLE=true
        log_info "cppcheck 已安装"
    else
        log_warning "cppcheck 未安装，跳过静态分析"
    fi
    
    # 检查gcov/lcov（可选）
    COVERAGE_AVAILABLE=false
    if command -v gcov &> /dev/null && command -v lcov &> /dev/null; then
        COVERAGE_AVAILABLE=true
        log_info "gcov/lcov 已安装"
    else
        log_warning "gcov/lcov 未安装，跳过代码覆盖率"
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "缺少必要依赖: ${missing_deps[*]}"
        exit 1
    fi
    
    log_success "依赖检查完成"
}

# 创建构建目录
setup_build_dir() {
    log_info "设置构建目录..."

    case "${BUILD_DIR}" in
        ""|"/") log_error "非法构建目录: ${BUILD_DIR}"; exit 1 ;;
    esac

    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
    mkdir -p "${REPORT_DIR}"
    
    log_success "构建目录已创建"
}

# CMake配置
cmake_configure() {
    log_info "CMake配置..."
    
    cd "${BUILD_DIR}"
    cmake "${AGENTRT_ROOT}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="--coverage -fprofile-arcs -ftest-coverage" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    
    log_success "CMake配置完成"
}

# 构建所有模块
build_all() {
    log_info "构建所有模块..."
    
    cd "${BUILD_DIR}"
    cmake --build . -- -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    
    log_success "构建完成"
}

# 运行单元测试
run_tests() {
    log_info "运行单元测试..."

    if ! command -v ctest &> /dev/null; then
        log_error "ctest 未安装"
        return 1
    fi

    cd "${BUILD_DIR}"
    # 单一构建树：ctest 统一执行全部注册测试，失败即失败（fail-closed）
    ctest --output-on-failure -j"$(nproc 2>/dev/null || echo 4)"

    log_success "所有测试通过"
}

# 运行cppcheck静态分析
run_cppcheck() {
    if [ "$CPPCHECK_AVAILABLE" = false ]; then
        log_warning "跳过cppcheck静态分析"
        return 0
    fi
    
    log_info "运行cppcheck静态分析..."
    
    local CPPCHECK_REPORT="${REPORT_DIR}/cppcheck_report.xml"
    
    cppcheck \
        --enable=all \
        --std=c11 \
        --platform=unix64 \
        --xml \
        --xml-version=2 \
        --output-file="${CPPCHECK_REPORT}" \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --check-level=exhaustive \
        --error-exitcode=1 \
        "${BACKS_DIR}" 2>/dev/null

    log_success "cppcheck静态分析完成"
}

# 运行代码覆盖率分析
run_coverage() {
    if [ "$COVERAGE_AVAILABLE" = false ]; then
        log_warning "跳过代码覆盖率分析"
        return 0
    fi
    
    log_info "运行代码覆盖率分析..."
    
    cd "${BUILD_DIR}"
    
    # 生成覆盖率数据
    lcov --capture --directory . --output-file coverage.info
    
    # 过滤系统头文件
    lcov --remove coverage.info '/usr/*' --output-file coverage.info
    
    # 生成HTML报告
    genhtml coverage.info --output-directory "${REPORT_DIR}/coverage"
    
    # 提取覆盖率百分比
    local coverage_percent=$(lcov --summary coverage.info 2>&1 | grep lines | grep -oP '\d+\.\d+%' | head -1)
    
    log_info "代码覆盖率: ${coverage_percent}"
    
    # 检查是否达到80%（fail-closed：未达标即失败）
    local coverage_value=$(echo "$coverage_percent" | sed 's/%//')
    if awk -v v="$coverage_value" 'BEGIN { exit !(v >= 80.0) }'; then
        log_success "代码覆盖率达标 (>= 80%)"
    else
        log_error "代码覆盖率未达标 (< 80%): ${coverage_percent}"
        return 1
    fi
    
    log_success "代码覆盖率报告已生成: ${REPORT_DIR}/coverage/"
}

# 生成构建报告
generate_report() {
    log_info "生成构建报告..."
    
    local REPORT_FILE="${REPORT_DIR}/build_report.txt"
    
    {
        echo "========================================"
        echo "AgentRT Backs模块构建报告"
        echo "========================================"
        echo ""
        echo "构建时间: $(date)"
        echo "构建目录: ${BUILD_DIR}"
        echo ""
        echo "--- 模块状态 ---"
        for module in "${DAEMON_MODULES[@]}"; do
            if [ -d "${BUILD_DIR}/daemons/${module}" ]; then
                echo "  ${module}: ✓ 已构建"
            else
                echo "  ${module}: ✗ 未构建"
            fi
        done
        echo ""
        echo "--- 测试状态 ---"
        if [ -f "${REPORT_DIR}/coverage/index.html" ]; then
            echo "  覆盖率报告: ✓ 已生成"
        else
            echo "  覆盖率报告: ✗ 未生成"
        fi
        echo ""
        echo "--- 静态分析 ---"
        if [ -f "${REPORT_DIR}/cppcheck_report.xml" ]; then
            local error_count=$(grep -c '<error' "${REPORT_DIR}/cppcheck_report.xml" 2>/dev/null || echo "0")
            echo "  cppcheck错误数: ${error_count}"
        else
            echo "  cppcheck: 未运行"
        fi
        echo ""
        echo "========================================"
    } > "${REPORT_FILE}"
    
    log_success "构建报告已生成: ${REPORT_FILE}"
}

# 清理构建产物
clean() {
    log_info "清理构建产物..."
    
    rm -rf "${BUILD_DIR}"
    rm -rf "${REPORT_DIR}"
    
    log_success "清理完成"
}

# 主函数
main() {
    local action="${1:-all}"
    
    case "$action" in
        all)
            check_dependencies
            setup_build_dir
            cmake_configure
            build_all
            run_tests
            run_cppcheck
            run_coverage
            generate_report
            ;;
        build)
            check_dependencies
            setup_build_dir
            cmake_configure
            build_all
            ;;
        test)
            run_tests
            ;;
        cppcheck)
            run_cppcheck
            ;;
        coverage)
            run_coverage
            ;;
        clean)
            clean
            ;;
        *)
            echo "用法: $0 {all|build|test|cppcheck|coverage|clean}"
            exit 1
            ;;
    esac
}

main "$@"
