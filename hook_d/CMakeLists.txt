# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Hook 守护进程构建文件
# M3 (0.1.9 §4.2-2): hook 系统核心已拆独立库 airy_coreloop_hooks。
# hook_d 只链接口库（registry/executor/interceptor/timeout/handlers），
# 不再链接整个认知引擎 airy_coreloopthree——消费者减重。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_COVERAGE "Build with code coverage" OFF)

# ==================== 路径设置 ====================
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# ==================== Hook daemon 可执行文件 ====================
# M3 (0.1.9 §4.2-2): hook 系统核心已拆独立库 airy_coreloop_hooks
# （registry/executor/interceptor/timeout/handlers，源码位于
# atoms/coreloopthree/src/hook/，target 定义见 coreloopthree/CMakeLists.txt）。
# hook_d 仅链接口库 airy_coreloop_hooks，不再链接整个认知引擎
# airy_coreloopthree——消费者减重（link-whitelist L39 登记）。
# hook_d 仅保留 main.c（daemon 进程入口：socket + SD + IPC + cupolas 守护进程生命周期）。
add_executable(hook_d
    src/main.c
)

target_compile_definitions(hook_d PRIVATE AIRY_ENABLE_V2_API=1)

if(WIN32)
    target_compile_definitions(hook_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(hook_d PRIVATE ws2_32 bcrypt)
elseif(UNIX)
    target_compile_definitions(hook_d PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
    target_link_libraries(hook_d PRIVATE Threads::Threads airy_platform_libs)
endif()

target_include_directories(hook_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cupolas/include
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${YAML_INCLUDE_DIRS}
)

# 链接策略：LTO + ASan 下使用 --start-group/--end-group 避免符号扫描遗漏
# M3: airy_coreloop_hooks（hook 系统核心）替代 airy_coreloopthree 全量引擎
if(NOT MSVC AND NOT APPLE)
    target_link_libraries(hook_d PRIVATE
        "-Wl,--start-group"
        airy_coreloop_hooks
        svc_common
        cupolas
        "-Wl,--end-group"
        Threads::Threads
    )
else()
    target_link_libraries(hook_d PRIVATE airy_coreloop_hooks svc_common cupolas Threads::Threads)
endif()

if(AIRY_HAS_YAML AND YAML_FOUND AND YAML_LIBRARIES)
    target_link_libraries(hook_d PRIVATE ${YAML_LIBRARIES})
endif()

if(AIRY_HAS_CURL)
    target_compile_definitions(hook_d PRIVATE AIRY_HAS_CURL=1)
    if(CURL_INCLUDE_DIRS)
        target_include_directories(hook_d PRIVATE ${CURL_INCLUDE_DIRS})
    endif()
    if(CURL_LIBRARIES)
        target_link_libraries(hook_d PRIVATE ${CURL_LIBRARIES})
    elseif(LIBCURL_LIBRARIES)
        target_link_libraries(hook_d PRIVATE ${LIBCURL_LIBRARIES})
    elseif(PC_CURL_LIBRARIES)
        target_link_libraries(hook_d PRIVATE ${PC_CURL_LIBRARIES})
    else()
        target_link_libraries(hook_d PRIVATE curl)
    endif()
endif()

# 编译选项
if(BUILD_COVERAGE)
    target_compile_options(hook_d PRIVATE
        -Wall -Wextra -Wpedantic -fprofile-arcs -ftest-coverage -g -O0)
    target_link_options(hook_d PRIVATE --coverage)
else()
    target_compile_options(hook_d PRIVATE
        -Wall -Wextra -Wpedantic
        -Werror=implicit-function-declaration -Werror=return-type
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
endif()

if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

install(TARGETS hook_d DESTINATION bin)
