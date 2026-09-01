# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Memory 服务单元测试构建文件


set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)

set(MEM_TEST_SOURCES
    test_service.c
    test_cache.c
    test_ledger.c
    test_compress.c
)

set(MEM_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error
    ${COMMONS_DIR}/utils/logging
    ${COMMONS_DIR}/utils/config_unified
    ${COMMONS_DIR}/utils/memory
    ${COMMONS_DIR}/utils/string
    ${COMMONS_DIR}/utils/sync
    ${COMMONS_DIR}/utils/cache
    ${COMMONS_DIR}/utils/compat
    ${SYSCALL_INCLUDE_DIR}
    ${COREKERN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
)

foreach(TEST_SRC ${MEM_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_mem_${TEST_NAME} ${TEST_SRC})

    target_include_directories(test_mem_${TEST_NAME} PRIVATE ${MEM_COMMON_INCLUDES})

    # 链接 airy_mem_service 静态库（已含 service.c + mem_svc_adapter.c）
    target_link_libraries(test_mem_${TEST_NAME} PRIVATE
        airy_mem_service
        svc_common
        airy_common
        Threads::Threads
        airy_platform_libs
    )

    if(AIRY_HAS_CJSON)
        target_link_libraries(test_mem_${TEST_NAME} PRIVATE cjson)
    endif()
    if(AIRY_HAS_YAML)
        target_link_libraries(test_mem_${TEST_NAME} PRIVATE yaml)
    endif()

    target_compile_options(test_mem_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME mem_d_${TEST_NAME} COMMAND test_mem_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
