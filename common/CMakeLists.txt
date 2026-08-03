# Copyright (c) 2026 SPHARX. All Rights Reserved. "From data intelligence emerges."
# daemon 公共库构建文件 - 最小兼容层
#
# 本模块现在作为 commons 统一基础库的兼容层，
# 提供向后兼容的 API，实际实现来自 commons。
#
# 保留的 daemon 特有模块：
# - svc_common.h: 服务框架接口
# - ipc_client.c: IPC 客户端实现


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_COVERAGE "Build with code coverage" OFF)

set(COMMONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../commons")
if(NOT EXISTS "${COMMONS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "commons module not found at ${COMMONS_DIR}. Please ensure commons is available.")
endif()
if(NOT TARGET airy_common)
    add_subdirectory(${COMMONS_DIR} ${CMAKE_BINARY_DIR}/commons)
endif()

set(ATOMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../atoms")

set(SVC_COMMON_SOURCES
    src/svc_common.c          # 服务框架核心实现
    src/ipc_client.c
    src/ipc_service_bus.c     # IPC服务总线
    src/ipc_bus_helper.c       # C-L09: daemon IPC Bus 自动注册便捷层
    src/service_discovery.c   # 跨进程服务发现
    src/service_discovery_helper.c  # C-L08: daemon 自动注册便捷层
    src/circuit_breaker.c     # 熔断器与自愈
    src/unified_metrics.c     # 统一指标收集
    src/alert_manager.c       # 智能告警管理
    src/config_manager.c      # 统一配置管理
    src/svc_auth.c
    src/daemon_security.c
    src/jsonrpc_helpers.c
    src/method_dispatcher.c
    src/input_validator.c
    src/param_validator.c
    src/log_sanitizer.c
    src/platform_compat.c     # 平台兼容层实现
    src/airy_event_loop.c  # 事件驱动循环
    src/daemon_event_driver.c # 统一事件驱动框架
    src/api_recovery.c        # API恢复系统
    src/thread_pool.c         # 工作线程池
    src/daemon_task_dispatcher.c # 工具并行执行引擎（原 parallel_dispatcher.c，重命名以消除与 atoms/coreloopthree 同名冲突）
    src/daemon_oom.c          # P1.22: Daemon OOM 降级回调注册
    src/ipc_backpressure.c    # P1.24: IPC Bus 背压控制
    src/daemon_bootstrap_sd.c    # P1.7 C-L08: daemon ServiceDiscovery 一键引导
    src/daemon_bootstrap_ipc.c   # P1.7 C-L09: daemon IPC Bus 一键引导
    src/daemon_cupolas_bootstrap.c # P3.14 ACC-DT15: daemon 统一 cupolas 安全穹顶引导
    src/daemon_rpc_client.c       # Phase 3: gateway→daemon thin IPC client (Unix socket JSON-RPC)
)

# 创建静态库
add_library(svc_common STATIC ${SVC_COMMON_SOURCES})

# 链接 commons 库
target_link_libraries(svc_common PUBLIC
    airy_common
    airy_compile_defs
)

# P3.14 ACC-DT15: cupolas 安全穹顶（PUBLIC 使得所有链接 svc_common 的 daemon 自动获得 cupolas）
if(TARGET cupolas)
    target_link_libraries(svc_common PUBLIC cupolas)
    target_include_directories(svc_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../cupolas/include>
    )
else()
    message(WARNING "cupolas target not found — daemon_cupolas_bootstrap will fail to link. "
                    "Ensure cupolas module is built (agentrt/cupolas/CMakeLists.txt).")
endif()
if(AIRY_HAS_OPENSSL)
    target_link_libraries(svc_common PUBLIC ${OPENSSL_CRYPTO_LIBRARY})
endif()

# 平台特定配置
if(WIN32)
    target_compile_definitions(svc_common PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(svc_common PRIVATE
        ws2_32
        bcrypt
        advapi32
    )
elseif(APPLE)
    target_compile_definitions(svc_common PRIVATE
        AIRY_PLATFORM_MACOS=1
    )
elseif(UNIX)
    target_compile_definitions(svc_common PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
    target_link_libraries(svc_common PRIVATE
        Threads::Threads
        airy_platform_libs
    )
endif()

# P0.17 IRON-6 跨层耦合禁令修复:
# commons 权威路径必须声明在 daemons/common/include 之前。
# CMake 的 INTERFACE_INCLUDE_DIRECTORIES 按声明顺序累加，并传递给所有链接 svc_common
# 的目标（包括 atoms/coreloopthree 的 airy_execution、airy_cognition 子目标）。
# 若 daemons/common/include 在前，atoms 源代码 #include "daemon_bootstrap_ipc.h" 等
# 会先命中 daemons 重导出兼容头（虽然内部经相对路径到达 commons，但仍构成编译期
# atoms→daemons 反向依赖）。将 daemons/common/include 移至 commons 路径之后，
# 确保 atoms 直接解析 commons 权威版本，完全消除 IRON-6 违规。
# 注意：daemons/common/include 中仍有未迁移的头文件（platform.h/error.h/compat.h/
# circuit_breaker.h/thread_pool.h 等），daemons 自己的源代码仍可通过此路径解析。
target_include_directories(svc_common PUBLIC
    # commons 权威路径（优先）
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/compat/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}>
    $<BUILD_INTERFACE:${COMMONS_DIR}/platform/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/error/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/logging/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/config_unified/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/memory/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/string/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/sync/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/observability/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/io/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/types/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/cache/include>
    # 新增网络和IPC模块
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/network/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/ipc/include>
    # P0.17 阶段 4: service_discovery.h/service_discovery_helper.h/daemon_bootstrap_sd.h
    # 迁移至 commons/utils/sd/include（daemons 版保留为重导出兼容头）
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/sd/include>
    # P0.17 阶段 1: daemon_defaults.h 重导出 airy_defaults.h（位于 commons/include）
    $<BUILD_INTERFACE:${COMMONS_DIR}/include>
    # daemons/common/include 兼容层路径（放最后，避免 atoms 误命中重导出兼容头）
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/agentrt>
)

target_include_directories(svc_common PUBLIC
    $<BUILD_INTERFACE:${ATOMS_DIR}/coreloopthree/include>
    $<BUILD_INTERFACE:${ATOMS_DIR}/coreloopthree/src/cognition>
    $<BUILD_INTERFACE:${ATOMS_DIR}/coreloopthree/src/cognition/critique>
    $<BUILD_INTERFACE:${ATOMS_DIR}/coreloopthree/src/cognition/foundation>
    $<BUILD_INTERFACE:${ATOMS_DIR}/memory>
    $<BUILD_INTERFACE:${ATOMS_DIR}/memory/src>
    # W18: taskflow_advanced.h 被 loop.h 包含（DAG 工作流集成），
    # orchestrator.c 包含 loop.h 因此间接依赖 taskflow 类型定义
    $<BUILD_INTERFACE:${ATOMS_DIR}/taskflow/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../daemons/llm_d/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../daemons/tool_d/include>
)
if(AIRY_HAS_CURL)
    target_compile_definitions(svc_common PUBLIC AIRY_HAS_CURL)
    if(CURL_FOUND)
        target_include_directories(svc_common PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(svc_common PUBLIC ${CURL_LIBRARIES})
    elseif(LIBCURL_FOUND)
        target_include_directories(svc_common PRIVATE ${LIBCURL_INCLUDE_DIRS})
        target_link_libraries(svc_common PUBLIC ${LIBCURL_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML AND YAML_FOUND)
    target_include_directories(svc_common PRIVATE ${YAML_INCLUDE_DIRS})
    target_link_libraries(svc_common PUBLIC ${YAML_LIBRARIES})
endif()
if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(svc_common PUBLIC cjson)
    elseif(CJSON_FOUND)
        target_include_directories(svc_common PRIVATE ${CJSON_INCLUDE_DIRS})
        target_link_libraries(svc_common PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()

target_link_libraries(svc_common PUBLIC Threads::Threads)

# 编译选项
if(BUILD_COVERAGE)
    if(NOT MSVC)
    target_compile_options(svc_common PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -fprofile-arcs
        -ftest-coverage
        -g
        -O0
    )
    target_link_options(svc_common PRIVATE
        --coverage
    )
    endif()
else()
    if(NOT MSVC)
    target_compile_options(svc_common PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror=implicit-function-declaration
        -Werror=return-type
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
    endif()
endif()

# 导出符号
set_target_properties(svc_common PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    POSITION_INDEPENDENT_CODE ON
)

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS svc_common
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# 安装兼容层头文件
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/svc_common.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/platform.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/error.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/svc_logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/svc_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/svc_cache.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/compat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/svc_auth.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_security.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/jsonrpc_helpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/method_dispatcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/input_validator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/param_validator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/log_sanitizer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ipc_service_bus.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ipc_bus_helper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/service_discovery.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/service_discovery_helper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/circuit_breaker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/unified_metrics.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alert_manager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/config_manager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/api_recovery.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/thread_pool.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_defaults.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_task_dispatcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/airy_event_loop.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_oom.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ipc_backpressure.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_startup.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_event_driver.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_cupolas_bootstrap.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_errors.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_platform_ext.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/daemon_rpc_client.h
    DESTINATION include/agentrt
)
