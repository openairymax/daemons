
# Channel 通道服务构建文件（channel.* 命名空间）
#
# gen5 SSoT 装配（.manifest → modules/sources.cmake）：入口 main.c /
# daemon 配置与端点策略 svc.c 由 daemon_gen.py 生成；RPC 方法域
# channel_rpc.c、簿记机制域 channel_book.c（双平台单源）、POSIX 策略
# channel_service.c 与收发域 channel_io.c 为手写业务源。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

# 平台映射（策略层）：簿记机制单源（2026-08-27 拆分，t89 簿记/传输再分）：
#   channel_book.c      生命周期/close/查询域（双平台单源，机制层）
#   channel_service.c   POSIX 三后端钩子 + SOCKET/SHM/PIPE 打开域（策略层）
#   channel_io.c        收发（send/receive）与连通性探测（ping）域
# Windows（G1 编译窗）：三种通道后端（AF_UNIX SOCKET / mkfifo PIPE /
# shm_open SHM）在 UCRT/airy_mman 下均未映射（#124 C1083 实证），改用
# channel_win32.c 注入钩子空实现（open/send/receive 显式 NOT_SUPPORTED），
# 与 HTTP/2 网关 Windows 禁用、run_stream 501 stub 同策略；传输映射完成后
# 删除本段并恢复 manifest 源清单直用。
if(WIN32)
    list(FILTER CHANNEL_D_SOURCES EXCLUDE REGEX "channel_(service|io)\\.c")
    list(APPEND CHANNEL_D_SOURCES src/channel_win32.c)
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
    add_executable(test_channel_e2e tests/test_channel_e2e.c src/channel_service.c src/channel_book.c src/channel_io.c)
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
    add_executable(test_channel_e2e tests/test_channel_e2e.c src/channel_service.c src/channel_book.c src/channel_io.c)
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
