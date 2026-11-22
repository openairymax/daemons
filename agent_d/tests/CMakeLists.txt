# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Agent 服务单元测试构建文件


set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)

set(AGENT_TEST_SOURCES
    test_service.c
    test_run_ledger.c
    test_run_loop.c
    test_run_open.c
    test_run_cancel.c
    test_run_stream.c
    test_task_orchestrate.c
    test_task_ledger.c
    test_turn_ckpt.c
    test_turn_rec.c
    test_agent_review.c
    test_invoke_payload.c
)

# 引擎闭环符号：white-box include 的测试已自带被包含单元，其余单元在此同编。
# agent_run_loop.c / agent_run_exec.c 互斥地由 test_run_loop / test_run_open
# 以 include 方式引入，避免同符号重复定义。
set(AGENT_RUN_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_ledger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_task_ledger.c
    # C-25: 等价工具表（run loop 换路判定）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_alt.c
    # D-12: 工具边界检查点（run loop 调用 pend/done）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_turn_ckpt.c
    # C-17: exec 收尾依赖判据派生与复核入口
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_verify.c
    # R16: exec 收尾的语义复核挂载点
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_review.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_task_orchestrate.c
    # S-P0-05: 每角色记忆记录由 exec 收尾调用，白盒测试须同编该单元
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_turn_rec.c
)

set(AGENT_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/run
    ${COMMONS_DIR}
    ${COMMONS_DIR}/include
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error
    ${COMMONS_DIR}/utils/logging
    ${COMMONS_DIR}/utils/config_unified
    ${COMMONS_DIR}/utils/memory
    ${COMMONS_DIR}/utils/string
    ${COMMONS_DIR}/utils/sync
    ${COMMONS_DIR}/utils/cache
    ${COMMONS_DIR}/utils/compat
    ${COMMONS_DIR}/utils/ipc
    ${COMMONS_DIR}/utils/hall
    ${SYSCALL_INCLUDE_DIR}
    ${COREKERN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
)

foreach(TEST_SRC ${AGENT_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_agent_${TEST_NAME} ${TEST_SRC})

    # white-box include 引擎源的测试需同编引擎闭环符号（engine/ledger/stream）。
    if(TEST_NAME STREQUAL "test_run_loop")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${AGENT_RUN_CORE_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_exec.c
        )
    endif()

    # 失败载荷契约直测：包含 exec 源，需同编其调用的工具循环闭环。
    if(TEST_NAME STREQUAL "test_run_open")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${AGENT_RUN_CORE_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_loop.c
        )
    endif()

    # R11 取消契约直测：包含工具循环源，需同编其调用的执行入口闭环。
    if(TEST_NAME STREQUAL "test_run_cancel")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${AGENT_RUN_CORE_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_run_exec.c
        )
    endif()

    # 裁决/身份单测同编编排源（不含 RPC/引擎，保持纯判定可测）；
    # C-17 判据归因同编复核入口（判据缺失路径不触达 RPC）。
    if(TEST_NAME STREQUAL "test_task_orchestrate")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_task_orchestrate.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_verify.c
        )
    endif()

    # 账本单测同编事件写入面：真实 hall_event 目录/信封，不 stub。
    if(TEST_NAME STREQUAL "test_task_ledger")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_task_ledger.c
        )
    endif()

    # R16 语义复核挂载点直测：只同编挂载点，不起引擎闭环。
    if(TEST_NAME STREQUAL "test_agent_review")
        target_sources(test_agent_${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/run/agent_review.c
        )
    endif()

    target_include_directories(test_agent_${TEST_NAME} PRIVATE ${AGENT_COMMON_INCLUDES})

    target_link_libraries(test_agent_${TEST_NAME} PRIVATE
        airy_agent_service
        svc_common
        airy_common
        Threads::Threads
        airy_platform_libs
    )

    if(AIRY_HAS_CJSON)
        target_link_libraries(test_agent_${TEST_NAME} PRIVATE cjson)
    endif()

    target_compile_options(test_agent_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME agent_d_${TEST_NAME} COMMAND test_agent_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
