# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Hook 守护进程 JSON-RPC 冒烟测试

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(CUPOLAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../cupolas)

set(HOOK_TEST_SOURCES
    test_hook_daemon.c
)

set(HOOK_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CUPOLAS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error
    ${COMMONS_DIR}/utils/logging
    ${COMMONS_DIR}/utils/config_unified
    ${COMMONS_DIR}/utils/memory
    ${COMMONS_DIR}/utils/string
    ${COMMONS_DIR}/utils/sync
    ${COMMONS_DIR}/utils/compat
    ${YAML_INCLUDE_DIRS}
)

foreach(TEST_SRC ${HOOK_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_hook_${TEST_NAME} ${TEST_SRC})

    target_include_directories(test_hook_${TEST_NAME} PRIVATE ${HOOK_COMMON_INCLUDES})

    target_compile_definitions(test_hook_${TEST_NAME} PRIVATE
        TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin")

    target_link_libraries(test_hook_${TEST_NAME} PRIVATE
        airy_common svc_common Threads::Threads airy_platform_libs
    )

    target_compile_options(test_hook_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME hook_d_${TEST_NAME} COMMAND test_hook_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
