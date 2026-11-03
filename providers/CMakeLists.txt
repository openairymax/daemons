# Copyright (c) 2026 SPHARX. All Rights Reserved. "From data intelligence emerges."
# LLM 传输机制库构建文件（0.1.20 §13.17.45 批次七）
#
# 能力格定位：线格式适配与出网传输的机制全集——registry 装配、密钥面、
# HTTP/SSE 传输、重试与限流、端点健康、流式 tool_calls 装配、四槽适配器
# 与三条线格式实现。本库不含任何策略面（路由选择、链式切换、配置装载、
# RPC 方法），故 llm_d 与未来其他消费方可独立链接。
#
# 与 daemons/common 同判据：非 daemon 机制库，不产可执行文件，不注册
# daemon 节点，不入 daemon 链接白名单（白名单只约束 daemon 目标）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ==================== 路径设置 ====================
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# core/ 机制件与 adapters/ 线格式实现
set(AIRY_TRANSPORT_SOURCES
    # 装配与身份
    src/core/registry.c
    # 密钥面（provider 凭据解析与抹除）
    src/core/secrets.c
    # 出网传输：URL/头链/超时/出站体
    src/core/http.c
    # 请求装配与响应解析信封
    src/core/envelope.c
    # 流式解析（SSE 帧）
    src/core/sse.c
    # 退避重试循环壳（http 非流式 / sse 流式共用）
    src/core/retry.c
    # 流式 tool_calls 装配 SSoT
    src/core/toolstream.c
    # 通用 completion driver——适配层四槽入口的唯一编排实现
    src/core/adapter.c
    # 调用方并发预算
    src/core/rate_limit.c
    # 上游失败线程局域诊断槽
    src/core/prov_diag.c
    # 端点健康探测机制件
    src/core/ep_health.c
    # 传输侧观测（超时三拆计数 + 时延 p99）
    src/core/prov_stat.c
    # 线格式适配（chat / messages / local + 装配表 SSoT）
    src/adapters/chat.c
    src/adapters/messages.c
    src/adapters/local.c
    src/adapters/adapter_table.c
)

add_library(airy_llm_transport STATIC ${AIRY_TRANSPORT_SOURCES})

if(WIN32)
    target_compile_definitions(airy_llm_transport PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_llm_transport PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

# 契约面：airy_prov/ 下 19 个头是跨模块唯一可见面；实现细节（src/）不传播。
target_include_directories(airy_llm_transport PUBLIC
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
    ${CURL_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
)

target_link_libraries(airy_llm_transport PUBLIC svc_common)

if(AIRY_HAS_CURL)
    if(CURL_FOUND AND CURL_LIBRARIES)
        target_link_libraries(airy_llm_transport PUBLIC ${CURL_LIBRARIES})
    elseif(LIBCURL_FOUND AND LIBCURL_LIBRARIES)
        target_link_libraries(airy_llm_transport PUBLIC ${LIBCURL_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_llm_transport PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_llm_transport PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()

if(UNIX)
    target_link_libraries(airy_llm_transport PUBLIC Threads::Threads airy_platform_libs)
endif()

if(NOT MSVC)
if(BUILD_COVERAGE)
    target_compile_options(airy_llm_transport PRIVATE
        -Wall -Wextra -Wpedantic -fprofile-arcs -ftest-coverage -g -O0
    )
else()
    target_compile_options(airy_llm_transport PRIVATE
        -Wall -Wextra -Wpedantic
        -Werror=implicit-function-declaration
        -Werror=return-type
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
endif()
endif()
