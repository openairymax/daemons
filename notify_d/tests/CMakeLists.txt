# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Notify 服务单元测试构建文件

set(NOTIFY_IMPL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/notify_service.c
)

set(NOTIFY_TEST_SOURCES
    test_notify_service.c
)

set(NOTIFY_TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
)

foreach(TEST_SRC ${NOTIFY_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_notify_${TEST_NAME} ${TEST_SRC} ${NOTIFY_IMPL_SOURCES})

    target_include_directories(test_notify_${TEST_NAME} PRIVATE ${NOTIFY_TEST_INCLUDES})

    target_link_libraries(test_notify_${TEST_NAME} PRIVATE
        svc_common
        airy_common
        Threads::Threads
        airy_platform_libs
    )

    if(AIRY_HAS_CJSON)
        target_link_libraries(test_notify_${TEST_NAME} PRIVATE cjson)
    endif()

    target_compile_options(test_notify_${TEST_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-function
        # Release 配置下 CMAKE_C_FLAGS_RELEASE 携带 -DNDEBUG，会编译掉 assert()，
        # 使测试空转。强制恢复断言语义，保证任意构建类型下测试均真实生效。
        -UNDEBUG
    )

    add_test(NAME notify_d_${TEST_NAME} COMMAND test_notify_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# ---- R7 并户：hook 面 JSON-RPC 冒烟测试（原 hook_d/tests 母本直迁）----
# 独立可执行：仅测试源（不编 notify 实现与 hook 面实现），spawn 真实
# notify_d 二进制经 hook.sock 驱动全链路
set(HOOK_FACE_TEST_SOURCES test_hook_daemon.c)

set(HOOK_FACE_TEST_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/compat
)

add_executable(test_hook_daemon ${HOOK_FACE_TEST_SOURCES})

target_include_directories(test_hook_daemon PRIVATE ${HOOK_FACE_TEST_INCLUDES})

target_compile_definitions(test_hook_daemon PRIVATE
    TEST_BIN_DIR="${CMAKE_BINARY_DIR}/bin")

target_link_libraries(test_hook_daemon PRIVATE
    airy_common svc_common Threads::Threads airy_platform_libs
)

target_compile_options(test_hook_daemon PRIVATE
    -Wall -Wextra -g -Wno-unused-function -UNDEBUG)

add_test(NAME notify_d_hook_daemon COMMAND test_hook_daemon
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
