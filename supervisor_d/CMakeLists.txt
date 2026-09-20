# Copyright (c) 2026 SPHARX. All Rights Reserved.
# supervisor_d 构建文件（0.1.18 §12.13 B13）

# V13.5 硬约束：零项目内库链接（link-whitelist.txt 允许集为空，linkgate
# 构建期 fail-closed）——UDS/TCP、进程原语、JSON、日志全部自持，仅链接
# 系统库：POSIX 纯 libc，Windows ws2_32。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SUPERVISOR_D_SOURCES
    src/main.c
    src/decl.c
    src/proc.c
    src/probe.c
    src/ctrl.c
)

add_executable(supervisor_d ${SUPERVISOR_D_SOURCES})

# 全局 "-include banned_functions.h" 在源码首行前处理，豁免只能由构建
# 系统注入（atoms/corekern/freestanding 同一先例）：supervisor_d 是零
# 依赖自持实现层，禁用面之外的纪律（snprintf/fgets/strtol/strtok_r）
# 自行保持。
if(AIRY_COMPLIANCE_STRICT)
    target_compile_definitions(supervisor_d PRIVATE AIRY_COMPLIANCE_IMPL)
endif()

if(WIN32)
    target_compile_definitions(supervisor_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(supervisor_d PRIVATE ws2_32)
endif()

target_include_directories(supervisor_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(NOT MSVC)
target_compile_options(supervisor_d PRIVATE
    -Wall
    -Wextra
    -Werror
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

install(TARGETS supervisor_d DESTINATION bin)

# 单元测试（复用顶层 BUILD_TESTS 开关）
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()
