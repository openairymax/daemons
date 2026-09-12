

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# channel_service.c 按单一职责拆分（2026-08-27）：
#   channel_service.c   生命周期/打开/关闭/查询域
#   channel_io.c        收发（send/receive）与连通性探测（ping）域
# Windows（G1 编译窗）：三种通道后端（AF_UNIX SOCKET / mkfifo PIPE /
# shm_open SHM）在 UCRT/airy_mman 下均未映射（#124 C1083 实证），改用
# channel_win32.c 纯内存服务层 stub（open/send/receive 显式 NOT_SUPPORTED），
# 与 HTTP/2 网关 Windows 禁用、run_stream 501 stub 同策略；传输映射完成后
# 恢复下方 POSIX 源清单并删除 win32 stub。
if(WIN32)
    set(CHANNEL_D_SOURCES
        src/main.c
        src/channel_win32.c
    )
else()
    set(CHANNEL_D_SOURCES
        src/main.c
        src/channel_service.c
        src/channel_io.c
    )
endif()

add_executable(channel_d ${CHANNEL_D_SOURCES})

target_include_directories(channel_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/compat
)

# 平台无关链接：svc_common/airy_common/Threads 为三平台共有，ws2_32 等
# 平台库由 airy_platform_libs 统一映射（Windows 分支此前缺失链接 → 构建失败）。
target_link_libraries(channel_d PRIVATE
    svc_common
    airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
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

# P25：channel 链测试 Windows 排除（与 daemons 顶层中央关闭双保险）；
# 测试源直引 sys/socket.h/AF_UNIX，MSVC 无对应运行语义（wave-4 #110）。
if(BUILD_TESTS AND NOT WIN32)
if(AIRY_HAS_CMOCKA)
    enable_testing()
    add_executable(test_channel_e2e tests/test_channel_e2e.c src/channel_service.c src/channel_io.c)
    target_compile_definitions(test_channel_e2e PRIVATE AIRY_COMPLIANCE_IMPL)
    target_include_directories(test_channel_e2e PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${COMMONS_INCLUDE_DIR}/utils/string
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
        ${COMMONS_INCLUDE_DIR}/utils/string
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
