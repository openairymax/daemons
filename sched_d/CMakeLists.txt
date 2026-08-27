# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Scheduler 服务构建文件


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 查找依赖
# Threads found by root CMakeLists.txt
# PkgConfig found by root CMakeLists.txt
# cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
if(NOT AIRY_HAS_CJSON)
    airy_print_warn("libcjson not found, JSON features may be limited")
endif()

# ==================== 路径设置 ====================
# agentrt/daemons/common 兼容层路径
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

# commons 统一基础库路径
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# Scheduler 服务源文件
set(SCHED_D_SOURCES
    src/main.c
    src/monitor.c
    src/roadmap_rpc.c
    # sched_service_impl.c / sched_dag_impl.c / main.c 已按功能域拆分
    # （单一职责），新增域文件如下：
    src/sched_service_impl.c
    src/sched_service_agent.c
    src/sched_service_task.c
    src/sched_service_worker.c
    src/sched_dag_impl.c
    src/sched_dag_engine.c
    src/sched_dag_worker.c
    src/sched_dag_parse.c
    src/sched_rpc_handlers.c
    src/sched_dispatch.c
    src/sched_svc_adapter.c
    src/strategies/weighted.c
    src/strategies/round_robin.c
    src/strategies/ml_based.c
    src/strategies/priority_based.c
)

# 创建可执行文件
add_executable(sched_d ${SCHED_D_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(sched_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(sched_d PRIVATE
        airy_common
        ws2_32
        bcrypt
    )
elseif(UNIX)
    target_link_libraries(sched_d PRIVATE
        airy_common
        Threads::Threads
        airy_platform_libs
    )
endif()

# 包含目录 - 兼容层和 commons
target_include_directories(sched_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error/include
    ${COMMONS_INCLUDE_DIR}/utils/logging/include
    ${COMMONS_INCLUDE_DIR}/utils/config_unified/include
    ${COMMONS_INCLUDE_DIR}/utils/memory/include
    ${COMMONS_INCLUDE_DIR}/utils/string/include
    ${COMMONS_INCLUDE_DIR}/utils/sync/include
    ${COMMONS_INCLUDE_DIR}/utils/cache/include
    ${COMMONS_INCLUDE_DIR}/utils/compat/include
    # 新增网络和IPC模块
    ${COMMONS_INCLUDE_DIR}/utils/network/include
    ${COMMONS_INCLUDE_DIR}/utils/ipc/include
    ${CJSON_INCLUDE_DIRS}
)

# 链接库
# coreloopthree 的 cognition 引擎（engine.c）引用 hook/config 子系统符号，
# 需 whole-archive 强制包含 coreloopthree 全部 .o（GNU ld 按序扫描，否则
# airy_cognition 库被拉取后其 hook/config 引用无法回头解析，见 tool_d 同款修复）。
target_link_libraries(sched_d PRIVATE svc_common)
if(UNIX AND NOT APPLE)
    target_link_libraries(sched_d PRIVATE
        -Wl,--whole-archive
        airy_coreloopthree
        -Wl,--no-whole-archive)
else()
    target_link_libraries(sched_d PRIVATE airy_coreloopthree)
endif()
target_link_libraries(sched_d PRIVATE
    airy_cognition
    airy_core
    airy_memory
    airy_atoms
    airy_common
    airy_llm_service
    airy_tool_service
    Threads::Threads
    airy_platform_libs
)

# DAG 并行派发（mac_framework 委派模式）：multi_agent_collaboration.h 位于
# coreloopthree/include，airy_rt.h 位于 corekern/include（head include 链）。
target_include_directories(sched_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../atoms/coreloopthree/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../atoms/corekern/include
)

# airy_coreloopthree → airy_core 会 PUBLIC 传播 AIRY_USE_SCHEDULER_THREAD_IMPL
# （corekern 调度器线程实现）。sched_d 未初始化 corekern scheduler，撤销该宏
# 恢复 platform.h 的 airy_thread_create → airy_platform_thread_create 宏路径
# （commons 平台线程实现，随 airy_common 链接），与既有线程用法保持一致。
target_compile_options(sched_d PRIVATE -UAIRY_USE_SCHEDULER_THREAD_IMPL)

# 编译选项
target_compile_options(sched_d PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS sched_d DESTINATION bin)
