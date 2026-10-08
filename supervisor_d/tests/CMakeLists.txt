# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# supervisor_d 为零依赖自持单元（不链接任何项目内库）：测试直接编入
# 被测三源件（不含 main.c），仅链系统库（POSIX libc / Windows ws2_32）。
add_executable(sup_test_supervisor_core test_supervisor_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/decl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/proc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ctrl.c
)

target_include_directories(sup_test_supervisor_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

if(WIN32)
    target_link_libraries(sup_test_supervisor_core PRIVATE ws2_32)
endif()

# GNU 风格警告旗标：MSVC 不识别 -W*（D8021，windows-build #113 实证）
if(NOT MSVC)
    target_compile_options(sup_test_supervisor_core PRIVATE
        -Wall -Wextra -g -Wno-unused-function -Wno-unused-variable
    )
endif()

add_test(NAME sup_test_supervisor_core COMMAND sup_test_supervisor_core
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

airy_print_ok("  [test] sup_test_supervisor_core: supervisor_d core unit tests configured")
