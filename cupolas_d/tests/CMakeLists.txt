# Copyright (c) 2026 SPHARX. All Rights Reserved.

# cupolas_d 服务层测试：经 airy_cupolas_service（STATIC，PUBLIC 面传递
# svc_common / cupolas / cjson-yaml / Threads / platform 及全部 include）
# 链接 service.c 与 cupolas 库真实实现；daemon_cupolas_init 全量引导
# （permission + sanitizer + audit + vault + entitlements + network），
# 严禁桩实现（IRON-2）。

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)

add_executable(cupolas_d_test_service test_cupolas_service.c)

target_include_directories(cupolas_d_test_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${COMMONS_DIR}/utils/error
    ${COMMONS_DIR}/utils/types
)

target_link_libraries(cupolas_d_test_service PRIVATE
    airy_cupolas_service
    Threads::Threads
)

# GNU 风格警告旗标：MSVC 不识别 -W*（D8021，windows-build #113 实证）
if(NOT MSVC)
    target_compile_options(cupolas_d_test_service PRIVATE
        -Wall -Wextra -g -Wno-unused-function
    )
endif()

add_test(NAME cupolas_d_test_service COMMAND cupolas_d_test_service
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

airy_print_ok("  [test] cupolas_d_test_service: cupolas_d service-level tests configured")
