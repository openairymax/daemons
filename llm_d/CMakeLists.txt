# Copyright (c) 2026 SPHARX. All Rights Reserved.
# LLM 服务构建文件


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 测试支持选项
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_COVERAGE "Build with code coverage" OFF)

# 查找依赖
# Threads found by root CMakeLists.txt
# PkgConfig found by root CMakeLists.txt
# cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
# CURL found by root CMakeLists.txt (AIRY_HAS_CURL)
if(NOT AIRY_HAS_CURL)
    airy_print_warn("libcurl not found, LLM remote API features may be limited")
endif()
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

# ==================== LLM 服务静态库 ====================
# 提取 service/providers/cache/cost_tracker/token_counter/response/llm_svc_adapter
# 为独立静态库，供 llm_d 可执行文件、atoms/coreloopthree 测试和集成测试共用，
# 消除桩函数依赖。排除 main.c（含 main()）。
set(LLM_SERVICE_SOURCES
    src/service.c
    src/config/service_config.c
    # service_config.c 域拆分（2026-08-27：1159 行 → 核心 + 5 个职责模块；
    # yaml 四件整体受 #ifdef HAVE_YAML 保护，非 YAML 构建下为空翻译单元，无条件列出无害）
    src/config/service_config_json.c
    src/config/service_config_yaml.c
    src/config/service_config_yaml_models.c
    src/config/service_config_yaml_providers.c
    src/service_config_yaml_pricing.c
    src/service_providers.c
    src/service_request.c
    src/accounting/service_metrics.c
    src/accounting/cache.c
    src/accounting/cost_tracker.c
    src/accounting/token_counter.c
    src/response.c
    # B16-S3：providers 域内重排 core/（机制）+ adapters/（适配）
    src/providers/core/registry.c
    # B16-S3 c3：provider.c 两分消失——base 生命周期归 registry.c，密钥机制归 secrets.c
    src/providers/core/secrets.c
    src/providers/core/http.c
    src/providers/core/envelope.c
    src/providers/core/sse.c
    # B16-S3 c8：退避重试循环壳（http 非流式 / sse 流式共用）
    src/providers/core/retry.c
    # B16-S3 c5：流式 tool_calls 装配 SSoT（三份同构收敛）
    src/providers/core/toolstream.c
    # B16-S3.3：通用 completion driver——适配层四槽入口的唯一编排实现
    src/providers/core/adapter.c
    # B16-S3 c6：openai 三件合一（openai.c + openai_stream.c + openai_internal.h → openai.c）
    src/providers/adapters/openai.c
    src/providers/core/rate_limit.c
    src/providers/adapters/anthropic.c
    # B16-S4：deepseek.c / google.c 清理（OpenAI 兼容遗留冗余，经 model.yaml base_url 声明式接入）
    src/providers/adapters/local.c
    # B16-S3 c7：适配器装配 SSoT——注册新适配的唯一改动点
    src/providers/adapters/adapter_table.c
    src/router/llm_router.c
    src/router/cost_aware_router.c
    src/router/round_robin_router.c
    src/router/least_latency_router.c
    src/router/quality_first_router.c
    src/llm_svc_adapter.c
    # 注入 atoms 侧 LLM ops 抽象表（服务实现在本库，表存储在 airy_llm_ops）
    src/daemon_llm_ops_bootstrap.c
)

add_library(airy_llm_service STATIC ${LLM_SERVICE_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(airy_llm_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_llm_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

# 包含目录 - 兼容层和 commons
# B16-S0 传播面收敛（V16.13①）：src/ 是内部实现细节，严禁 PUBLIC 传播
target_include_directories(airy_llm_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/include
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/config_unified
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/cache
    ${COMMONS_INCLUDE_DIR}/utils/compat
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
)

target_include_directories(airy_llm_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/providers
    # B16-S5 预置：顶层文件五域归位后的头搜索路径（纯重排，bare-name include 不变）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/accounting
    ${CURL_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接库：svc_common 提供 daemon 公共层 + commons 传递依赖
target_link_libraries(airy_llm_service PUBLIC svc_common)

# daemon_llm_ops_bootstrap 注入 atoms 侧 LLM ops 抽象表。
# ops 存储（are_ops_set/get_llm）在独立小库 airy_llm_ops 中，而非 airy_llm_service
# 自身——否则 atoms 侧消费 are_ops_get_llm 时需反向链接 airy_llm_service，构成
# AIRY-02/IRON-6 违规。PRIVATE 链接保证最终可执行文件链接行含该归档；
# 接口 include（airy_llm_ops.h）由 svc_common PUBLIC 传递的
# atoms/coreloopthree/include 提供。
target_link_libraries(airy_llm_service PRIVATE airy_llm_ops)

if(AIRY_HAS_CURL)
    if(CURL_FOUND AND CURL_LIBRARIES)
        target_link_libraries(airy_llm_service PUBLIC ${CURL_LIBRARIES})
    elseif(LIBCURL_FOUND AND LIBCURL_LIBRARIES)
        target_link_libraries(airy_llm_service PUBLIC ${LIBCURL_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_llm_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_llm_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML)
    target_link_libraries(airy_llm_service PUBLIC ${YAML_LIBRARIES})
    target_compile_definitions(airy_llm_service PRIVATE HAVE_YAML=1)
endif()

if(UNIX)
    target_link_libraries(airy_llm_service PUBLIC Threads::Threads airy_platform_libs)
endif()

# 编译选项
if(NOT MSVC)
if(BUILD_COVERAGE)
    target_compile_options(airy_llm_service PRIVATE
        -Wall -Wextra -Wpedantic -fprofile-arcs -ftest-coverage -g -O0
    )
else()
    target_compile_options(airy_llm_service PRIVATE
        -Wall -Wextra -Wpedantic
        -Werror=implicit-function-declaration
        -Werror=return-type
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
endif()
endif()

# ==================== LLM daemon 可执行文件 ====================
# main.c 域拆分（2026-08-27：1033 行 → 纯入口 + 3 个 daemon 模块）。
# daemon 四件共享 DAEMON_DECLARE_COMMON 宏实例化产生的 static 符号，仅随
# llm_d 可执行目标构建，不入 airy_llm_service 静态库（main.c 同理）。
add_executable(llm_d
    src/main.c
    src/llm_daemon_request.c
    src/llm_daemon_methods.c
    src/llm_daemon_config.c
)

if(WIN32)
    target_compile_definitions(llm_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(llm_d PRIVATE ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(llm_d PRIVATE Threads::Threads airy_platform_libs)
endif()

target_include_directories(llm_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # B16-S5 预置：五域归位后的头搜索路径（与 airy_llm_service 同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/accounting
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
    ${COMMONS_INCLUDE_DIR}/utils/network
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CURL_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接策略：LTO + ASan 下使用 --start-group/--end-group 避免符号扫描遗漏
if(NOT MSVC AND NOT APPLE)
    target_link_libraries(llm_d PRIVATE
        "-Wl,--start-group"
        airy_llm_service
        "-Wl,--end-group"
        airy_core # WS-8 8.4.1: corekern link for airy_init() in main.c
        Threads::Threads
    )
else()
    target_link_libraries(llm_d PRIVATE airy_llm_service airy_core Threads::Threads)
endif()

if(NOT MSVC)
if(BUILD_COVERAGE)
    target_compile_options(llm_d PRIVATE
        -Wall -Wextra -Wpedantic -fprofile-arcs -ftest-coverage -g -O0
    )
    target_link_options(llm_d PRIVATE --coverage)
else()
    target_compile_options(llm_d PRIVATE
        -Wall -Wextra -Wpedantic
        -Werror=implicit-function-declaration
        -Werror=return-type
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
endif()
endif()

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS llm_d DESTINATION bin)
