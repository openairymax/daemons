

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# channel_service.c 按单一职责拆分（2026-08-27）：
#   channel_service.c   生命周期/打开/关闭/查询域
#   channel_io.c        收发（send/receive）与连通性探测（ping）域
set(CHANNEL_D_SOURCES
    src/main.c
    src/channel_service.c
    src/channel_io.c
)

add_executable(channel_d ${CHANNEL_D_SOURCES})

target_include_directories(channel_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error/include
    ${COMMONS_INCLUDE_DIR}/utils/logging/include
    ${COMMONS_INCLUDE_DIR}/utils/memory/include
    ${COMMONS_INCLUDE_DIR}/utils/string/include
    ${COMMONS_INCLUDE_DIR}/utils/sync/include
    ${COMMONS_INCLUDE_DIR}/utils/compat/include
)

# 平台无关链接：svc_common/airy_common/Threads 为三平台共有，ws2_32 等
# 平台库由 airy_platform_libs 统一映射（Windows 分支此前缺失链接 → 构建失败）。
target_link_libraries(channel_d PRIVATE
    svc_common
    airy_common
    Threads::Threads
    airy_platform_libs
)
# R-09-119: Linux FHS 路径宏仅在 Linux 设置；macOS 使用 platform.h 的
# 默认路径（./agentrt/config、./agentrt/logs），避免覆盖后无写权限。
if(UNIX AND NOT APPLE)
    target_compile_definitions(channel_d PRIVATE
        _GNU_SOURCE
        AIRY_CONFIG_DIR="/etc/agentrt"
        AIRY_LOG_DIR="/var/log/agentrt"
    )
endif()

if(NOT MSVC)
target_compile_options(channel_d PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

install(TARGETS channel_d
    RUNTIME DESTINATION bin
)

install(FILES
    include/channel_service.h
    DESTINATION include/agentrt/daemons/channel_d
)

if(BUILD_TESTS)
if(AIRY_HAS_CMOCKA)
    enable_testing()
    add_executable(test_channel_e2e tests/test_channel_e2e.c src/channel_service.c src/channel_io.c)
    target_compile_definitions(test_channel_e2e PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_channel_e2e PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COMMONS_INCLUDE_DIR}/utils/string/include
    )
    target_link_libraries(test_channel_e2e PRIVATE
        ${CMOCKA_LIBRARIES}
        svc_common
        airy_common
        Threads::Threads airy_platform_libs
    )
    target_compile_options(test_channel_e2e PRIVATE -Wall -Wextra)
    add_test(NAME channel_e2e COMMAND test_channel_e2e)
else()
    enable_testing()
    add_executable(test_channel_e2e tests/test_channel_e2e.c src/channel_service.c src/channel_io.c)
    target_compile_definitions(test_channel_e2e PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_channel_e2e PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COMMONS_DIR}/tests/utils
        ${COMMONS_INCLUDE_DIR}/utils/string/include
    )
    target_link_libraries(test_channel_e2e PRIVATE Threads::Threads airy_platform_libs svc_common airy_common)
    target_compile_options(test_channel_e2e PRIVATE -Wall -Wextra)
    target_compile_definitions(test_channel_e2e PRIVATE
        USE_CMOCKA_STUB
    )
    if(NOT APPLE)
        target_compile_definitions(test_channel_e2e PRIVATE
            # R-09-119: Linux FHS 路径宏仅在 Linux 设置（macOS 用 platform.h 默认）
            AIRY_CONFIG_DIR="/etc/agentrt"
            AIRY_LOG_DIR="/var/log/agentrt"
        )
    endif()
    add_test(NAME channel_e2e COMMAND test_channel_e2e)
endif()
endif() # BUILD_TESTS
