# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Tool 服务构建文件


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 查找依赖
# Threads found by root CMakeLists.txt
# PkgConfig found by root CMakeLists.txt
# cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
if(NOT AIRY_HAS_CJSON)
    airy_print_warn("libcjson not found, JSON features may be limited")
endif()

# ==================== 路径设置 ====================
# agentrt/daemons/common 兼容层路径
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

# commons 统一基础库路径
set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# ==================== Tool 服务静态库 ====================
# 提取 service/registry/executor/validator/cache/tool_approval/safety_guard_bridge/
# tool_svc_adapter 为独立静态库，供 tool_d 可执行文件、atoms/coreloopthree
# 测试和集成测试共用，消除桩函数依赖。
# 排除 main.c（含 main()）、config.c、utils/tool_helpers.c（daemon 专用工具函数）。
# service.c 按单一职责拆分（2026-08-27）：
#   service.c            生命周期/注册表/统计/资源释放/交互审批域
#   service_builtin.c    builtin 工具注册 + AIRY_AGENT_ACL 预授权域
#   service_execute.c    工具执行域（同步/流式 + 缓存）
set(TOOL_SERVICE_SOURCES
    src/service.c
    src/service_builtin.c
    src/service_execute.c
    src/registry.c
    src/executor.c
    src/validator.c
    src/cache.c
    src/tool_approval.c
    src/safety_guard_bridge.c
    src/tool_svc_adapter.c
    src/builtin.c
    src/builtin_shell.c
    src/builtin_fs.c
    src/builtin_fs_dir.c
    src/builtin_fs_glob.c
    src/builtin_fs_grep.c
    src/builtin_net.c
    src/builtin_git.c
    src/builtin_maths.c
    src/tool_interactive_approval.c
    src/os_sandbox.c
    # 0.1.9 M4：plugin_d → tool_d 整编——插件 dlopen 引擎随迁
    src/plugin_service.c
    src/plugin_discovery.c
    src/plugin_permission.c
)

add_library(airy_tool_service STATIC ${TOOL_SERVICE_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(airy_tool_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_tool_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

# 包含目录 - 兼容层和 commons
target_include_directories(airy_tool_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    # C-L05: Cupolas SafetyGuard 头文件路径
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cupolas/include
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    # P3.18 (ACC-DT27): sandbox 公共 API（airy_sandbox.h）和 syscall 号（syscalls.h）
    ${CMAKE_CURRENT_SOURCE_DIR}/../../atoms/syscall/include
)

target_include_directories(airy_tool_service PRIVATE
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接库：svc_common 提供 daemon 公共层，cupolas 提供 SafetyGuard
# P3.18 (ACC-DT27): airy_syscall 提供 sandbox 公共 API（airy_sandbox_invoke 等）
# 依赖链：airy_tool_service → airy_syscall → airy_coreloopthree → airy_core
# 无循环依赖（coreloopthree 不链接 tool_service，仅 include 头文件）
target_link_libraries(airy_tool_service PUBLIC svc_common cupolas airy_syscall)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_tool_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_tool_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML AND YAML_FOUND AND YAML_LIBRARIES)
    target_link_libraries(airy_tool_service PUBLIC ${YAML_LIBRARIES})
endif()

if(UNIX)
    target_link_libraries(airy_tool_service PUBLIC Threads::Threads airy_platform_libs)
endif()

# 编译选项
target_compile_options(airy_tool_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# C-L05: 启用 Cupolas SafetyGuard V2 API
target_compile_definitions(airy_tool_service PRIVATE AIRY_ENABLE_V2_API=1)

# ==================== Tool daemon 可执行文件 ====================
# main.c 仅属于 daemon 可执行文件；config.c 和 utils/tool_helpers.c 为 daemon 专用工具函数；
# tool_svc_adapter.c 已纳入 airy_tool_service 库
add_executable(tool_d
    src/main.c
    src/config.c
    src/utils/tool_helpers.c
    # 0.1.9 M4：plugin_d → tool_d 整编——插件 RPC 方法面
    src/plugin_rpc.c
)

if(WIN32)
    target_compile_definitions(tool_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(tool_d PRIVATE airy_common ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(tool_d PRIVATE airy_common Threads::Threads airy_platform_libs)
    # dlopen 插件加载：GNU/Linux libdl（glibc≥2.34 并入 libc）；macOS
    # dlopen 在 libSystem，CMAKE_DL_LIBS 自动为空
    target_link_libraries(tool_d PRIVATE ${CMAKE_DL_LIBS})
endif()

target_include_directories(tool_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cupolas/include
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# P3.18 (ACC-DT27): airy_tool_service → airy_syscall → airy_coreloopthree
# 传递依赖。coreloopthree 引用 cognition/llm/checkpoint/hook_d 等符号，
# 需 whole-archive 强制包含 coreloopthree 全部 .o，且必须在 airy_llm_service
# 之前链接（GNU ld 按序扫描：coreloopthree 的 llm_service 引用需在后续
# 库中解析）。--whole-archive 为 GNU ld 专有（macOS ld64 / MSVC 不识别，
# 直接链接，循环符号由两平台链接器整体解析兜底）。
target_link_libraries(tool_d PRIVATE airy_tool_service)
if(UNIX AND NOT APPLE)
    target_link_libraries(tool_d PRIVATE
        -Wl,--whole-archive
        airy_coreloopthree
        -Wl,--no-whole-archive)
else()
    target_link_libraries(tool_d PRIVATE airy_coreloopthree)
endif()
target_link_libraries(tool_d PRIVATE
    airy_cognition
    airy_core
    airy_memory
    airy_atoms
    airy_common
    airy_llm_service
    Threads::Threads
    airy_platform_libs
)

target_compile_options(tool_d PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

target_compile_definitions(tool_d PRIVATE AIRY_ENABLE_V2_API=1)

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS tool_d DESTINATION bin)
