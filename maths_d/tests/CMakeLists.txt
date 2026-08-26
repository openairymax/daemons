# Copyright (c) 2026 SPHARX. All Rights Reserved.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(MATHS_TEST_SOURCES
    test_maths_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/maths_service.c
)

add_executable(test_maths_service ${MATHS_TEST_SOURCES})

target_include_directories(test_maths_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
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
