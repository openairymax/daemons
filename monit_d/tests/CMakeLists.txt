# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Monitor 服务测试构建文件


set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)

set(MONIT_IMPL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/service_subsystems.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/service_agent_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/alert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/logging.c
)

set(MONIT_TEST_SOURCES
    test_metrics.c
    test_alert.c
    test_tracing.c
)

set(MONIT_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
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

foreach(TEST_SRC ${MONIT_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_monit_${TEST_NAME} ${TEST_SRC} ${MONIT_IMPL_SOURCES})

    target_include_directories(test_monit_${TEST_NAME} PRIVATE ${MONIT_COMMON_INCLUDES})

    target_link_libraries(test_monit_${TEST_NAME} PRIVATE
        svc_common
        Threads::Threads
    )

    target_compile_options(test_monit_${TEST_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-function
    )

    add_test(NAME monit_d_${TEST_NAME} COMMAND test_monit_${TEST_NAME})
endforeach()

# M4-S2 内建模块测试：直接编译模块真实实现（observe 附编 prometheus_exporter
# 提供 um_* 融合导出符号；info 仅依赖 svc_common 传递链）
add_executable(test_monit_observe_rpc
    test_observe_rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/observe_rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/prometheus_exporter.c
)
target_include_directories(test_monit_observe_rpc PRIVATE ${MONIT_COMMON_INCLUDES})
target_link_libraries(test_monit_observe_rpc PRIVATE
    svc_common
    Threads::Threads
)
target_compile_options(test_monit_observe_rpc PRIVATE -Wall -Wextra -Wno-unused-function)
add_test(NAME monit_d_observe_rpc COMMAND test_monit_observe_rpc)

add_executable(test_monit_info_rpc
    test_info_rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/info_rpc.c
)
target_include_directories(test_monit_info_rpc PRIVATE ${MONIT_COMMON_INCLUDES})
target_link_libraries(test_monit_info_rpc PRIVATE
    svc_common
    Threads::Threads
)
target_compile_options(test_monit_info_rpc PRIVATE -Wall -Wextra -Wno-unused-function)
add_test(NAME monit_d_info_rpc COMMAND test_monit_info_rpc)

enable_testing()
