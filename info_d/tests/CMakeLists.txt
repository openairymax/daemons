# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Info 服务单元测试

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

add_executable(test_info_service test_info_service.c)

target_include_directories(test_info_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${COMMONS_INCLUDE_DIR}/utils/compat
)

target_link_libraries(test_info_service PRIVATE
    svc_common
    Threads::Threads
)

target_compile_options(test_info_service PRIVATE
    -Wall
    -Wextra
    -Wno-unused-function
)

add_test(NAME info_d_service COMMAND test_info_service)

enable_testing()
