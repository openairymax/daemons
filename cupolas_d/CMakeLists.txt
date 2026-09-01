# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Cupolas 安全穹顶服务构建文件（cupolas.* 命名空间）
#
# cupolas_d 将 cupolas 安全库拆分为独立 daemon，通过 Unix socket JSON-RPC
# 暴露 cupolas.* 命名空间方法（权限裁决/输入净化/命令执行/规则管理/审计）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 查找依赖
# Threads found by root CMakeLists.txt
# cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
if(NOT AIRY_HAS_CJSON)
    airy_print_warn("libcjson not found, JSON features may be limited")
endif()

# ==================== 路径设置 ====================
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

set(CUPOLAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../cupolas)
set(CUPOLAS_INCLUDE_DIR ${CUPOLAS_DIR}/include)

# ==================== Cupolas 服务静态库 ====================
# 提取 service/cupolas_svc_adapter 为独立静态库，供 cupolas_d 可执行文件
# 与单元测试共用，消除桩函数依赖。排除 main.c（含 main()）。
set(CUPOLAS_SERVICE_SOURCES
    src/service.c
    src/cupolas_svc_adapter.c
)

add_library(airy_cupolas_service STATIC ${CUPOLAS_SERVICE_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(airy_cupolas_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_cupolas_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

target_include_directories(airy_cupolas_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CUPOLAS_INCLUDE_DIR}
)

target_include_directories(airy_cupolas_service PRIVATE
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接库：svc_common 提供 daemon 公共层（PUBLIC 传递 cupolas），
# 显式链接 cupolas 安全库本体
target_link_libraries(airy_cupolas_service PUBLIC svc_common cupolas)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_cupolas_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_cupolas_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML AND YAML_FOUND AND YAML_LIBRARIES)
    target_link_libraries(airy_cupolas_service PUBLIC ${YAML_LIBRARIES})
endif()

if(UNIX)
    target_link_libraries(airy_cupolas_service PUBLIC Threads::Threads airy_platform_libs)
endif()

target_compile_options(airy_cupolas_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# ==================== Cupolas daemon 可执行文件 ====================
# main.c 按单一职责拆分（2026-08-27）：
#   main.c                          入口引导 + daemon 配置装配 + 方法注册
#   src/cupolas_rpc_core.c          cupolas.* 核心方法（权限/净化/命令/规则/审计/健康/统计）
#   src/cupolas_rpc_vault.c         cupolas.* vault 方法（含 hex 编解码）
#   src/cupolas_rpc_net_entitlements.c  cupolas.* net/entitlements 方法
add_executable(cupolas_d
    src/main.c
    src/cupolas_rpc_core.c
    src/cupolas_rpc_vault.c
    src/cupolas_rpc_net_entitlements.c
    src/cupolas_rpc_policy.c
)

# M2-S3 (0.1.9 §3.2 PDP)：policy.* RPC 需要 dynamic_policy_engine V2 API
target_compile_definitions(cupolas_d PRIVATE AIRY_ENABLE_V2_API=1)

if(WIN32)
    target_compile_definitions(cupolas_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(cupolas_d PRIVATE airy_common ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(cupolas_d PRIVATE airy_common Threads::Threads airy_platform_libs)
endif()

target_include_directories(cupolas_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CUPOLAS_INCLUDE_DIR}
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

target_link_libraries(cupolas_d PRIVATE
    airy_cupolas_service
    airy_common
    Threads::Threads
    airy_platform_libs
)

target_compile_options(cupolas_d PRIVATE
    -Wall -Wextra -Wpedantic
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
install(TARGETS cupolas_d DESTINATION bin)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)
