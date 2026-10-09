# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Cupolas 安全穹顶服务构建文件（cupolas.* 命名空间）
#
# cupolas_d 将 cupolas 安全库拆分为独立 daemon，通过 Unix socket JSON-RPC
# 暴露 cupolas.* 命名空间方法（权限裁决/输入净化/命令执行/规则管理/审计）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 0.1.19 §254b：构建位上移顶层装配区后，exe 输出目录随户自持，
# 与 daemons 约定（bin/）一致——CorekernRuntime 等门禁按 ${BUILD}/bin/ 取证。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

set(CUPOLAS_DIR "${CUPOLAS_SRC_DIR}")
set(CUPOLAS_INCLUDE_DIR ${CUPOLAS_DIR}/include)

# ==================== 安全穹顶策略单元（0.1.19 §254b） ====================
# airy_security_dome 是 daemon_security 机制核的 vault provider 策略实现
# （security_dome.c）：机制核（svc_common）已彻底去除 cupolas 直链与产品
# 头依赖，穹顶身份（vault id / 口令环境变量 / cupolas 存储后端）全部收拢
# 于本策略单元。PUBLIC 链 cupolas 既为 dome 自身取得 cupolas_vault.h 等
# src/security 产品头面，也向消费者传递（notify_d hook_svc.c 经此前置链
# 解析 safety_guard.h）。
add_library(airy_security_dome STATIC src/security_dome.c)

if(WIN32)
    target_compile_definitions(airy_security_dome PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_security_dome PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

target_include_directories(airy_security_dome PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 机制核（daemon_security.h / svc_logger.h 经其 PUBLIC include 面）+ 穹顶实现本体
target_link_libraries(airy_security_dome PUBLIC svc_common cupolas)

if(UNIX)
    target_link_libraries(airy_security_dome PUBLIC Threads::Threads airy_platform_libs)
endif()

if(NOT MSVC)
target_compile_options(airy_security_dome PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# ==================== Cupolas 服务静态库 ====================
# 提取 service 为独立静态库，供 cupolas_d 可执行文件
# 与单元测试共用，消除桩函数依赖。排除 main.c（含 main()）。
set(CUPOLAS_SERVICE_SOURCES
    src/service.c
)

add_library(airy_cupolas_service STATIC ${CUPOLAS_SERVICE_SOURCES})

# M2-S2（0.1.9 §3.2 PDP）：service 层裁决面引入 dpolicy overlay，需
# dynamic_policy_engine V2 API 声明（引擎实现随 cupolas 库以同宏编译）
target_compile_definitions(airy_cupolas_service PRIVATE AIRY_ENABLE_V2_API=1)

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

# 内部头仅自用：src 区不对外传播（0.1.19 F4 收敛）
target_include_directories(airy_cupolas_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(airy_cupolas_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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

# 链接库：经 airy_security_dome 策略单元传递取得 svc_common 机制核层与
# cupolas 产品面（0.1.19 §254b：service 不再直链 cupolas）
target_link_libraries(airy_cupolas_service PUBLIC airy_security_dome)

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

if(NOT MSVC)
target_compile_options(airy_cupolas_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# ==================== Cupolas daemon 可执行文件 ====================
# gen5 SSoT 装配（.manifest → modules/sources.cmake）：入口 main.c /
# daemon 配置与端点策略 svc.c 由 daemon_gen.py 生成；RPC 方法域
# cupolas_rpc_core/vault/net_entitlements/policy.c 为手写业务源。
# 服务域 service.c 单独编译为 airy_cupolas_service 静态库（上方）。
include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

add_executable(cupolas_d ${CUPOLAS_D_SOURCES})

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
    airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
    airy_common
    Threads::Threads
    airy_platform_libs
)

if(NOT MSVC)
target_compile_options(cupolas_d PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# 测试支持。P25：cupolas 测试 Windows 不开放——§254b 构建位上移后本目录
# 脱离 daemons 目录级 WIN32 开关作用域，改由本文件自持 NOT WIN32 守卫。
if(BUILD_TESTS AND NOT WIN32)
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
