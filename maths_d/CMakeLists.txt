# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Maths 数学外挂计算服务构建文件

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# 源清单由 gen5 SSoT 装配（.manifest → modules/sources.cmake）
include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

add_executable(maths_d ${MATHS_D_SOURCES})

if(WIN32)
    target_compile_definitions(maths_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(maths_d PRIVATE
        airy_common
        ws2_32
    )
elseif(UNIX)
    target_link_libraries(maths_d PRIVATE
        airy_common
        Threads::Threads
        airy_platform_libs
    )
endif()

target_include_directories(maths_d PRIVATE
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
)

target_link_libraries(maths_d PRIVATE
    svc_common
    airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
    Threads::Threads
)
# 0.1.19 §254b: boot 接线 vault 策略单元（daemon_dome_init_pep/_cleanup 符号面）
target_link_libraries(maths_d PRIVATE airy_security_dome)
# m（libm）：仅非 MSVC 平台存在；MSVC 数学函数内建在 CRT（wave-4 #110
# LNK1181 m.lib 防护，与根 airy_platform_libs 同判据）。
if(NOT MSVC)
    target_link_libraries(maths_d PRIVATE m)
endif()

if(NOT MSVC)
target_compile_options(maths_d PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

install(TARGETS maths_d DESTINATION bin)

if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()
