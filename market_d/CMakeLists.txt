# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Market 服务构建文件


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

# ==================== Market 服务静态库 ====================
# publisher/market_service 五域编译为独立静态库，
# 供 market_d 可执行文件与集成测试共用。
# 排除 main.c（含 main()）。
set(MARKET_SERVICE_SOURCES
    src/publisher.c
    src/market_service_impl.c
    src/market_service_registry.c  # 市场服务-注册域
    src/market_service_query.c     # 市场服务-只读查询域（检索/清单/更新检查）
    src/market_service_install.c   # 市场服务-安装/卸载域
    src/market_service_config.c    # 市场服务-配置/同步域
)

add_library(airy_market_service STATIC ${MARKET_SERVICE_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(airy_market_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_market_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

# 包含目录 - 兼容层和 commons
# 内部头仅自用：src 区不对外传播（0.1.19 F4 收敛）
target_include_directories(airy_market_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(airy_market_service PUBLIC
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
)

target_include_directories(airy_market_service PRIVATE
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接库：svc_common 提供 daemon 公共层 + commons 传递依赖
target_link_libraries(airy_market_service PUBLIC svc_common)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_market_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_market_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML AND YAML_FOUND AND YAML_LIBRARIES)
    target_link_libraries(airy_market_service PUBLIC ${YAML_LIBRARIES})
endif()

if(UNIX)
    target_link_libraries(airy_market_service PUBLIC Threads::Threads airy_platform_libs)
endif()

# 编译选项
if(NOT MSVC)
target_compile_options(airy_market_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# ==================== Market daemon 可执行文件 ====================
# gen5 SSoT 装配（.manifest → modules/sources.cmake）：入口 main.c /
# daemon 配置与端点策略 svc.c 由 daemon_gen.py 生成；RPC 方法域
# market_rpc.c 为手写业务源。服务域五源单独编译为 airy_market_service
# 静态库（上方），不入 ${MARKET_D_SOURCES}，避免重复编译。
include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

add_executable(market_d ${MARKET_D_SOURCES})

if(WIN32)
    target_compile_definitions(market_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(market_d PRIVATE ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(market_d PRIVATE Threads::Threads airy_platform_libs)
endif()

target_include_directories(market_d PRIVATE
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
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

target_link_libraries(market_d PRIVATE
    airy_market_service
    airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
    Threads::Threads
)

if(NOT MSVC)
target_compile_options(market_d PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS market_d DESTINATION bin)
