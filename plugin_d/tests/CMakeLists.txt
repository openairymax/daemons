# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# Plugin 权限模块单元测试

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(CUPOLAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../cupolas)

set(PLUGIN_IMPL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin_permission.c
    # P1-5 离线扩展校验器测试：需编译 discovery 实现（依赖
    # plugin_service_load，由 service.c 提供）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin_discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/service.c
)

set(PLUGIN_TEST_SOURCES
    test_plugin_permission.c
    test_plugin_discovery.c
)

set(PLUGIN_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CUPOLAS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error/include
    ${COMMONS_DIR}/utils/logging/include
    ${COMMONS_DIR}/utils/config_unified/include
    ${COMMONS_DIR}/utils/memory/include
    ${COMMONS_DIR}/utils/string/include
    ${COMMONS_DIR}/utils/sync/include
    ${COMMONS_DIR}/utils/compat/include
    ${YAML_INCLUDE_DIRS}
)

foreach(TEST_SRC ${PLUGIN_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_plugin_${TEST_NAME} ${TEST_SRC} ${PLUGIN_IMPL_SOURCES})

    # safety_guard V2 API 需显式启用
    target_compile_definitions(test_plugin_${TEST_NAME} PRIVATE AIRY_ENABLE_V2_API=1)

    target_include_directories(test_plugin_${TEST_NAME} PRIVATE ${PLUGIN_COMMON_INCLUDES})

    target_link_libraries(test_plugin_${TEST_NAME} PRIVATE
        airy_common svc_common cupolas Threads::Threads airy_platform_libs dl
        ${YAML_LIBRARIES}
    )

    target_compile_options(test_plugin_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME plugin_d_${TEST_NAME} COMMAND test_plugin_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

enable_testing()
