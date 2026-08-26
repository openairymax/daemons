# Copyright (c) 2026 SPHARX. All Rights Reserved.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(MATHS_TEST_SOURCES
    test_maths_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/maths_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/expr_eval.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/python_backend.c
)

add_executable(test_maths_service ${MATHS_TEST_SOURCES})

set(DAEMON_COMMON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include)
set(COMMONS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)

target_include_directories(test_maths_service PRIVATE
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
    ${COMMONS_INCLUDE_DIR}/utils/network/include
    ${COMMONS_INCLUDE_DIR}/utils/ipc/include
    ${COMMONS_INCLUDE_DIR}/utils/compat/include
)

target_link_libraries(test_maths_service PRIVATE
    svc_common
    Threads::Threads
    m
)

target_compile_options(test_maths_service PRIVATE
    -Wall
    -Wextra
)

add_test(NAME test_maths_service COMMAND test_maths_service)
