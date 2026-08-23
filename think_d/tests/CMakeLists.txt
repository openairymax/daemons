# Copyright (c) 2026 SPHARX. All Rights Reserved.
# think_d 测试构建文件：GCCP 两段式交互闭环（P-A）等

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

set(ATOMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../atoms)
set(CORELOOPTHREE_DIR ${ATOMS_DIR}/coreloopthree)

set(THINK_TEST_SOURCES
    test_think_gccp_twopass.c
)

foreach(TEST_SRC ${THINK_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_think_${TEST_NAME} ${TEST_SRC})

    target_include_directories(test_think_${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${DAEMON_COMMON_INCLUDE_DIR}
        ${COMMONS_INCLUDE_DIR}
        ${COMMONS_INCLUDE_DIR}/platform/include
        ${COMMONS_INCLUDE_DIR}/utils/error/include
        ${COMMONS_INCLUDE_DIR}/utils/logging/include
        ${COMMONS_INCLUDE_DIR}/utils/memory/include
        ${COMMONS_INCLUDE_DIR}/utils/string/include
        ${COMMONS_INCLUDE_DIR}/utils/sync/include
        ${COMMONS_INCLUDE_DIR}/utils/ipc/include
        ${CORELOOPTHREE_DIR}/include
        ${CJSON_INCLUDE_DIRS}
    )

    target_link_libraries(test_think_${TEST_NAME} PRIVATE
        airy_think_service
        airy_common
        Threads::Threads
        airy_platform_libs
    )

    if(AIRY_HAS_CJSON)
        target_link_libraries(test_think_${TEST_NAME} PRIVATE cjson)
    endif()

    # 与 think_d 主目标一致：coreloopthree ↔ cognition ↔ llm_service/
    # tool_service 静态库循环依赖，需 --start-group/--end-group（仅 GNU ld）
    if(NOT MSVC AND NOT APPLE)
        target_link_libraries(test_think_${TEST_NAME} PRIVATE
            "-Wl,--start-group"
            airy_coreloopthree
            airy_cognition
            airy_llm_service
            airy_tool_service
            "-Wl,--end-group"
        )
    else()
        target_link_libraries(test_think_${TEST_NAME} PRIVATE
            airy_coreloopthree
            airy_llm_service
            airy_tool_service
        )
    endif()

    target_compile_options(test_think_${TEST_NAME} PRIVATE
        -Wall -Wextra -g -Wno-unused-function
    )

    add_test(NAME think_d_${TEST_NAME} COMMAND test_think_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
