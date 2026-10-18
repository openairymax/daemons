# Gateway Daemon Tests (TeamC)

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Copyright (C) 2026 SPHARX. All Rights Reserved.



set(GATEWAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../gateway)
set(GATEWAY_INCLUDE_DIR ${GATEWAY_DIR}/include)
set(GATEWAY_SRC_DIR ${GATEWAY_DIR}/src)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)

# libmicrohttpd/libwebsockets found by root CMakeLists.txt

set(PROTOCOL_ROUTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/protocol)

set(TEST_SOURCES
    test_service.c
    test_protocol_router.c
    test_gateway_jsonpick.c
    test_gateway_hall_store.c
    test_gateway_cap_registry.c
    test_gateway_pep_cache.c
    bench_gateway_pep.c
    test_svcdispatch_e2e.c
)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    # svcdispatch e2e 直接链接 airy_gateway_service（含 svc.c 与业务面），
    # 再编译 svc.c 会与之重复定义。
    set(TEST_OWN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../src/svc.c)
    if(${TEST_NAME} STREQUAL "test_svcdispatch_e2e")
        set(TEST_OWN_SOURCES "")
    endif()

    add_executable(${TEST_NAME}
        ${TEST_SRC}
        ${TEST_OWN_SOURCES}
    )

    if(${TEST_NAME} STREQUAL "test_protocol_router")
        target_sources(${TEST_NAME} PRIVATE
            ${PROTOCOL_ROUTER_DIR}/gateway_protocol_router.c
            ${PROTOCOL_ROUTER_DIR}/gateway_jsonpick.c
            ${PROTOCOL_ROUTER_DIR}/gateway_mcp_server.c
            ${PROTOCOL_ROUTER_DIR}/gateway_a2a_handler.c
            ${PROTOCOL_ROUTER_DIR}/gateway_openai_compat.c
        )
        target_include_directories(${TEST_NAME} PRIVATE
            ${PROTOCOL_ROUTER_DIR}
        )
    endif()

    # JSON 提取机制单测：仅编译机制件（cJSON 头 + 库依赖）
    if(${TEST_NAME} STREQUAL "test_gateway_jsonpick")
        target_sources(${TEST_NAME} PRIVATE
            ${PROTOCOL_ROUTER_DIR}/gateway_jsonpick.c
        )
        target_include_directories(${TEST_NAME} PRIVATE
            ${PROTOCOL_ROUTER_DIR}
            ${CJSON_INCLUDE_DIRS}
        )
    endif()

    # 统一能力网关：注册表测试需编译注册表实现
    # （含 cJSON 头依赖），网关业务头不参与测试目标。
    # 0.1.19 刀H：业务面迁出至 products/gateway_biz（产品件）。
    if(${TEST_NAME} STREQUAL "test_gateway_cap_registry")
        target_sources(${TEST_NAME} PRIVATE
            ${GATEWAY_BIZ_SRC_DIR}/src/gateway_cap_registry.c
        )
        target_include_directories(${TEST_NAME} PRIVATE
            ${CJSON_INCLUDE_DIRS}
            # 0.1.19: gateway/.manifest 构建期生成的策略表
            ${GATEWAY_GEN_DIR}
        )
        add_dependencies(${TEST_NAME} gateway_policy_gen)
    endif()

    # PEP 裁定缓存测试——编译实现源；
    # gw_svc_call 符号由 airy_gateway_service（含业务面产品件）提供。
    # 依赖方向自上而下（gateway_service -> syscall），单遍左到右解析完备。
    # bench_gateway_pep（PEP 端到端基准）同一装配。
    if(${TEST_NAME} STREQUAL "test_gateway_pep_cache" OR
       ${TEST_NAME} STREQUAL "bench_gateway_pep")
        target_sources(${TEST_NAME} PRIVATE
            ${GATEWAY_BIZ_SRC_DIR}/src/gateway_pep_cache.c
        )
        target_include_directories(${TEST_NAME} PRIVATE
            ${CJSON_INCLUDE_DIRS}
        )
        target_link_libraries(${TEST_NAME} PRIVATE
            airy_gateway_service
            airy_gateway_biz
            airy_syscall
        )
    endif()

    # 0.1.19 §254b: PEP 缓存测试直调策略单元引导面
    # （daemon_dome_init_pep/_cleanup），链 airy_security_dome 并取其头面。
    # §255: cupolas_d 随产品壳迁出，头面经顶层 CUPOLAS_SRC_DIR 锚点取得。
    if(${TEST_NAME} STREQUAL "test_gateway_pep_cache")
        target_include_directories(${TEST_NAME} PRIVATE
            ${CUPOLAS_SRC_DIR}/daemon/include)
        target_link_libraries(${TEST_NAME} PRIVATE airy_security_dome)
    endif()

    # svcdispatch e2e：SYS_SVC_CALL -> 钩子 -> gw_svc_call -> mock daemon。
    # 被测面（gw_sys_svc_dispatch_* / gw_svc_call / 业务上下文）在服务库与
    # 业务面产品件内，二者均直链。
    if(${TEST_NAME} STREQUAL "test_svcdispatch_e2e")
        target_link_libraries(${TEST_NAME} PRIVATE
            airy_gateway_service
            airy_gateway_biz
        )
    endif()

    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
        ${GATEWAY_INCLUDE_DIR}
        ${GATEWAY_SRC_DIR}
        ${GATEWAY_SRC_DIR}/gateway
        # 0.1.19 刀H：业务面头面随产品件迁出
        ${GATEWAY_BIZ_SRC_DIR}/include
        ${GATEWAY_SRC_DIR}/utils
        ${SYSCALL_INCLUDE_DIR}
        ${COREKERN_INCLUDE_DIR}
        ${COMMONS_DIR}
        ${COMMONS_DIR}/include
        ${COMMONS_DIR}/platform/include
        ${COMMONS_DIR}/utils/include
        ${COMMONS_DIR}/utils/error
        ${COMMONS_DIR}/utils/logging
        ${COMMONS_DIR}/utils/config_unified
        ${COMMONS_DIR}/utils/memory
        ${COMMONS_DIR}/utils/string
        ${COMMONS_DIR}/utils/sync
        ${COMMONS_DIR}/utils/cache
        ${COMMONS_DIR}/utils/compat
        ${CJSON_INCLUDE_DIRS}
    )

    target_link_libraries(${TEST_NAME}
        PRIVATE
            svc_common
            Threads::Threads
    )

    if(AIRY_HAS_CJSON AND TARGET gateway_lib_obj AND AIRY_HAS_MICROHTTPD)
        # 架构约束 2026-08-25 "必须走 syscall": gateway 测试目标经
        # gateway_lib_obj（OBJECT 形式）引用 airy_sys_svc_call（atoms/syscall
        # svc_dispatch.o），不经 gateway 库目标故无 PUBLIC 传递，必须显式
        # 链接 airy_syscall。对象库直接参与链接无归档顺序问题，
        # 依赖方向自上而下（syscall -> protocols），单遍左到右解析完备。
        target_link_libraries(${TEST_NAME} PRIVATE
            $<TARGET_OBJECTS:gateway_lib_obj>
            airy_syscall
            airy_protocols
        )
    endif()

    if(AIRY_HAS_MICROHTTPD)
        if(libmicrohttpd_FOUND)
            target_link_libraries(${TEST_NAME} PRIVATE microhttpd)
        elseif(MICROHTTPD_FOUND)
            target_include_directories(${TEST_NAME} PRIVATE ${MICROHTTPD_INCLUDE_DIRS})
            target_link_libraries(${TEST_NAME} PRIVATE ${MICROHTTPD_LIBRARIES})
        endif()
    endif()

    if(AIRY_HAS_LIBWEBSOCKETS)
        # 优先自编译 4.3.3 的 websockets_shared imported target（libssl.so.3）
        if(TARGET websockets_shared)
            target_link_libraries(${TEST_NAME} PRIVATE websockets_shared)
        elseif(TARGET websockets)
            target_link_libraries(${TEST_NAME} PRIVATE websockets)
        elseif(LIBWEBSOCKETS_FOUND)
            target_include_directories(${TEST_NAME} PRIVATE ${LIBWEBSOCKETS_INCLUDE_DIRS})
            target_link_libraries(${TEST_NAME} PRIVATE ${LIBWEBSOCKETS_LIBRARIES})
        endif()
    endif()

    # nghttp2 链接（HTTP/2 支持）：gateway_lib_obj 含 http2_gateway.c，
    # 引用 nghttp2 符号，测试目标必须显式链接（与 gateway_d 主目标同模式，
    # 使用完整路径避免系统仅有 .so.14 无 .so 软链接时 -lnghttp2 找不到）。
    if(AIRY_HAS_HTTP2 AND NGHTTP2_LIBRARY)
        target_link_libraries(${TEST_NAME} PRIVATE ${NGHTTP2_LIBRARY})
    endif()

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME gateway_d_${TEST_NAME} COMMAND ${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
