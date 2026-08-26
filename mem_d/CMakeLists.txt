# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Memory 服务构建文件（mem.* 命名空间）


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 查找依赖
# Threads found by root CMakeLists.txt
# cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
if(NOT AIRY_HAS_CJSON)
    airy_print_warn("libcjson not found, JSON features may be limited")
endif()

# ==================== 路径设置 ====================
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

# ==================== Memory 服务静态库 ====================
# 提取 service/mem_svc_adapter 为独立静态库，供 mem_d 可执行文件与单元测试共用，
# 消除桩函数依赖。排除 main.c（含 main()）。
# vector.c 提供自研 TF-IDF 向量检索；emb_client.c 提供可选 embedding 后端（AIRY_HAS_CURL）
set(MEM_SERVICE_SOURCES
    src/service.c
    src/mem_svc_adapter.c
    src/vector.c
    src/emb_client.c
    src/cache.c
    src/ledger.c
    src/compress.c
)

add_library(airy_mem_service STATIC ${MEM_SERVICE_SOURCES})

# 平台特定配置
if(WIN32)
    target_compile_definitions(airy_mem_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_mem_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

target_include_directories(airy_mem_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error/include
    ${COMMONS_INCLUDE_DIR}/utils/logging/include
    ${COMMONS_INCLUDE_DIR}/utils/config_unified/include
    ${COMMONS_INCLUDE_DIR}/utils/memory/include
    ${COMMONS_INCLUDE_DIR}/utils/string/include
    ${COMMONS_INCLUDE_DIR}/utils/sync/include
    ${COMMONS_INCLUDE_DIR}/utils/cache/include
    ${COMMONS_INCLUDE_DIR}/utils/compat/include
    ${COMMONS_INCLUDE_DIR}/utils/network/include
    ${COMMONS_INCLUDE_DIR}/utils/ipc/include
    ${COMMONS_INCLUDE_DIR}/utils/token/include
)

target_include_directories(airy_mem_service PRIVATE
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

# 链接库：svc_common 提供 daemon 公共层
target_link_libraries(airy_mem_service PUBLIC svc_common)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_mem_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_mem_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()
if(AIRY_HAS_YAML AND YAML_FOUND AND YAML_LIBRARIES)
    target_link_libraries(airy_mem_service PUBLIC ${YAML_LIBRARIES})
endif()

# 可选 embedding 后端：libcurl（AIRY_HAS_CURL 由 dependencies.cmake 定义）
if(AIRY_HAS_CURL)
    if(CURL_FOUND)
        target_link_libraries(airy_mem_service PUBLIC CURL::libcurl)
    elseif(LIBCURL_FOUND)
        target_link_libraries(airy_mem_service PUBLIC ${LIBCURL_LIBRARIES})
        target_include_directories(airy_mem_service PUBLIC ${LIBCURL_INCLUDE_DIRS})
    else()
        message(WARNING "AIRY_HAS_CURL defined but CURL target not found, embedding backend disabled")
    endif()
endif()

if(UNIX)
    target_link_libraries(airy_mem_service PUBLIC Threads::Threads airy_platform_libs)
endif()

target_compile_options(airy_mem_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# ==================== Memory daemon 可执行文件 ====================
add_executable(mem_d
    src/main.c
)

if(WIN32)
    target_compile_definitions(mem_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(mem_d PRIVATE airy_common ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(mem_d PRIVATE airy_common Threads::Threads airy_platform_libs)
endif()

target_include_directories(mem_d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error/include
    ${COMMONS_INCLUDE_DIR}/utils/logging/include
    ${COMMONS_INCLUDE_DIR}/utils/config_unified/include
    ${COMMONS_INCLUDE_DIR}/utils/memory/include
    ${COMMONS_INCLUDE_DIR}/utils/string/include
    ${COMMONS_INCLUDE_DIR}/utils/sync/include
    ${COMMONS_INCLUDE_DIR}/utils/cache/include
    ${COMMONS_INCLUDE_DIR}/utils/compat/include
    ${COMMONS_INCLUDE_DIR}/utils/network/include
    ${COMMONS_INCLUDE_DIR}/utils/ipc/include
    ${COMMONS_INCLUDE_DIR}/utils/token/include
    ${CJSON_INCLUDE_DIRS}
    ${YAML_INCLUDE_DIRS}
)

target_link_libraries(mem_d PRIVATE
    airy_mem_service
    airy_common
    Threads::Threads
    airy_platform_libs
)

target_compile_options(mem_d PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS mem_d DESTINATION bin)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)
