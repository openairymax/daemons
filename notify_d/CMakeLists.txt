# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Notify 服务构建文件（0.1.19 五件套化，codegen=false 异型户：
# 单端口四协议多路复用，装配清单手工维护于 modules/sources.cmake）


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Threads found by root CMakeLists.txt

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

add_executable(notify_d ${NOTIFY_D_SOURCES})

# R7 并户：safety_guard.h 的 safety_guard_create 等声明位于
# AIRY_ENABLE_V2_API 门控块，与 hook_d 形态一致
target_compile_definitions(notify_d PRIVATE AIRY_ENABLE_V2_API=1)

if(WIN32)
    target_compile_definitions(notify_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(notify_d PRIVATE
        airy_common
        ws2_32
    )
elseif(UNIX)
    target_link_libraries(notify_d PRIVATE
        airy_common
        Threads::Threads
        airy_platform_libs
    )
endif()

target_include_directories(notify_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${COMMONS_INCLUDE_DIR}/utils/cjson # R7 并户：CJSON_PARSE_GUARD（hook_rpc.c）
)

target_link_libraries(notify_d PRIVATE
    svc_common
    airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
    airy_coreloop_hooks # R7 并户：hook registry/executor/builtin handlers
    Threads::Threads
)
# 0.1.19 §254b: boot 接线 vault 策略单元（daemon_dome_init_pep/_cleanup 符号
# 面）；hook_svc.c 的 safety_guard.h 头面改经 dome PUBLIC cupolas 传递取得。
target_link_libraries(notify_d PRIVATE airy_security_dome)

if(NOT MSVC)
target_compile_options(notify_d PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

install(TARGETS notify_d DESTINATION bin)

# 单元测试（复用顶层 BUILD_TESTS 开关）
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()
