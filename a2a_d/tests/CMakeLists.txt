# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# A2A 服务单元测试构建文件


set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)
set(PROTOCOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../protocols)

set(A2A_TEST_SOURCES
    test_service.c
)

set(A2A_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error/include
    ${COMMONS_DIR}/utils/logging/include
    ${COMMONS_DIR}/utils/config_unified/include
    ${COMMONS_DIR}/utils/memory/include
    ${COMMONS_DIR}/utils/string/include
    ${COMMONS_DIR}/utils/sync/include
    ${COMMONS_DIR}/utils/cache/include
    ${COMMONS_DIR}/utils/compat/include
    ${SYSCALL_INCLUDE_DIR}
    ${COREKERN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${PROTOCOLS_DIR}/standards/a2a/include
    ${PROTOCOLS_DIR}/include
    ${PROTOCOLS_DIR}/common/include
)

foreach(TEST_SRC ${A2A_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_a2a_${TEST_NAME} ${TEST_SRC})

    target_include_directories(test_a2a_${TEST_NAME} PRIVATE ${A2A_COMMON_INCLUDES})

    # 链接 airy_a2a_service 静态库（已含 service.c + a2a_svc_adapter.c）
    # 与 airy_protocols（提供 a2a_v03_adapter 实现）
    target_link_libraries(test_a2a_${TEST_NAME} PRIVATE
        airy_a2a_service
        airy_protocols
        svc_common
        airy_common
        Threads::Threads
        airy_platform_libs
    )

    if(AIRY_HAS_CJSON)
        target_link_libraries(test_a2a_${TEST_NAME} PRIVATE cjson)
    endif()
    if(AIRY_HAS_YAML)
        target_link_libraries(test_a2a_${TEST_NAME} PRIVATE yaml)
    endif()

    target_compile_options(test_a2a_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME a2a_d_${TEST_NAME} COMMAND test_a2a_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
