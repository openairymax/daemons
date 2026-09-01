# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Info 服务构建文件


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Threads found by root CMakeLists.txt

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

set(INFO_D_SOURCES
    src/main.c
)

add_executable(info_d ${INFO_D_SOURCES})

if(WIN32)
    target_compile_definitions(info_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(info_d PRIVATE
        airy_common
        ws2_32
    )
elseif(UNIX)
    target_link_libraries(info_d PRIVATE
        airy_common
        Threads::Threads
        airy_platform_libs
    )
endif()

target_include_directories(info_d PRIVATE
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

target_link_libraries(info_d PRIVATE
    svc_common
    Threads::Threads
)

target_compile_options(info_d PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

install(TARGETS info_d DESTINATION bin)

# ==================== 单元测试 ====================
if(BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()
