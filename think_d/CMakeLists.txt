# Copyright (c) 2026 SPHARX. All Rights Reserved.
# 双思考系统服务构建文件（think.* 命名空间）
#
# gen5 装配（L3 SSoT）：daemon 源清单由 modules/sources.cmake 声明
# （自 .manifest 生成）；机制层（main.c/svc_think_d.h）生成、策略层
# （src/svc.c、src/think_rpc.c、lang/review 服务面）手写。
# think_d 承载 CoreLoopThree 认知引擎（Thinkdual: t2/t1-f/t1-p +
# dual_coordinate），LLM 调用经 llm_svc_adapter 直连 llm_d Unix socket。
# 链接 airy_coreloopthree（其 PUBLIC 传递 airy_cognition/airy_core/
# airy_memory/airy_taskflow/svc_common 及 coreloopthree include 路径）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/modules/sources.cmake)

# ==================== 路径设置 ====================
set(DAEMON_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(DAEMON_COMMON_INCLUDE_DIR ${DAEMON_COMMON_DIR}/include)

set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../commons)
set(COMMONS_INCLUDE_DIR ${COMMONS_DIR})

set(ATOMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../atoms)
set(CORELOOPTHREE_DIR ${ATOMS_DIR}/coreloopthree)

# ==================== Think 服务静态库 ====================
set(THINK_SERVICE_SOURCES
    src/think_service.c
    # think_service.c 域拆分（2026-08-27：1067 行 → 2 个职责模块）
    src/think_orch.c
)

add_library(airy_think_service STATIC ${THINK_SERVICE_SOURCES})

if(WIN32)
    target_compile_definitions(airy_think_service PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
elseif(UNIX)
    target_compile_definitions(airy_think_service PRIVATE
        AIRY_PLATFORM_LINUX=1
        _GNU_SOURCE
    )
endif()

# 内部头仅自用：src 区不对外传播（0.1.19 F4 收敛）
target_include_directories(airy_think_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(airy_think_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DAEMON_COMMON_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}
    ${COMMONS_INCLUDE_DIR}/platform/include
    ${COMMONS_INCLUDE_DIR}/utils/error
    ${COMMONS_INCLUDE_DIR}/utils/logging
    ${COMMONS_INCLUDE_DIR}/utils/memory
    ${COMMONS_INCLUDE_DIR}/utils/string
    ${COMMONS_INCLUDE_DIR}/utils/sync
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CORELOOPTHREE_DIR}/include
)

target_include_directories(airy_think_service PRIVATE
    ${CJSON_INCLUDE_DIRS}
    # B16-S0：跨 daemon 头依赖显式化（svc_common 不再代传 llm_d/include）
    ${CMAKE_CURRENT_SOURCE_DIR}/../llm_d/include
)

# 核心依赖：CoreLoopThree 认知引擎（含 LLM 适配器、双思考、GCCP、WorkHall）
target_link_libraries(airy_think_service PUBLIC airy_coreloopthree svc_common)

if(AIRY_HAS_CJSON)
    if(cJSON_FOUND)
        target_link_libraries(airy_think_service PUBLIC cjson)
    elseif(CJSON_FOUND AND CJSON_LIBRARIES)
        target_link_libraries(airy_think_service PUBLIC ${CJSON_LIBRARIES})
    endif()
endif()

if(UNIX)
    target_link_libraries(airy_think_service PUBLIC Threads::Threads airy_platform_libs)
endif()

if(NOT MSVC)
target_compile_options(airy_think_service PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# ==================== Think daemon 可执行文件 ====================
add_executable(think_d ${THINK_D_SOURCES})

if(WIN32)
    target_compile_definitions(think_d PRIVATE
        AIRY_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(think_d PRIVATE airy_common ws2_32 bcrypt)
elseif(UNIX)
    target_link_libraries(think_d PRIVATE airy_common Threads::Threads airy_platform_libs)
endif()

target_include_directories(think_d PRIVATE
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
    ${COMMONS_INCLUDE_DIR}/utils/ipc
    ${CJSON_INCLUDE_DIRS}
)

# 依赖拓扑序（成员级 DAG，单遍左到右解析完备）：
# cognition/engine.o -> coreloopthree/gccp.o -> llm_service/llm_service_complete；
# cognition/engine.o -> tool_service/tool_result_free。
target_link_libraries(think_d PRIVATE
    airy_think_service
    airy_cognition
    airy_coreloopthree
    airy_llm_service
    airy_tool_service
    airy_common
    Threads::Threads
    airy_platform_libs
)
# 0.1.19 §254b: boot 接线 vault 策略单元（daemon_dome_init_pep/_cleanup 符号面）
target_link_libraries(think_d PRIVATE airy_security_dome)

if(NOT MSVC)
target_compile_options(think_d PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
    -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
endif()

# 测试支持
if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_subdirectory(tests)
    endif()
endif()

# 安装规则
install(TARGETS think_d DESTINATION bin)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)
