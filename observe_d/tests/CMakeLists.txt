# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Observe 服务单元测试构建文件
#
# observe_d 的实现全部位于 src/main.c（含 main()），测试通过
# #define main + #include "../src/main.c" 的方式复用全部 static 实现，
# 避免把服务代码拆库重构（保持 daemon 结构不变）。

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(OBSERVE_TEST_SOURCES
    test_observe_service.c
)

set(OBSERVE_TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error/include
    ${COMMONS_DIR}/utils/logging/include
    ${COMMONS_DIR}/utils/memory/include
    ${COMMONS_DIR}/utils/string/include
    ${COMMONS_DIR}/utils/sync/include
    ${COMMONS_DIR}/utils/network/include
    ${COMMONS_DIR}/utils/ipc/include
    ${COMMONS_DIR}/utils/compat/include
)

foreach(TEST_SRC ${OBSERVE_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_observe_${TEST_NAME} ${TEST_SRC})

    target_include_directories(test_observe_${TEST_NAME} PRIVATE ${OBSERVE_TEST_INCLUDES})

    target_link_libraries(test_observe_${TEST_NAME} PRIVATE
        svc_common
        airy_platform_libs
        Threads::Threads
        m
    )

    # svc_common PUBLIC 传递链接 cjson（AIRY_HAS_CJSON 时）
    if(AIRY_HAS_CJSON AND cJSON_FOUND)
        target_link_libraries(test_observe_${TEST_NAME} PRIVATE cjson)
    endif()

    target_compile_options(test_observe_${TEST_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-function
        -Wno-unused-parameter
        -Werror=implicit-function-declaration
        # 测试断言（assert）在 Release（-DNDEBUG）下默认失效：
        # 取消 NDEBUG，保证测试始终真正执行校验
        -UNDEBUG
    )

    add_test(NAME observe_d_${TEST_NAME} COMMAND test_observe_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
