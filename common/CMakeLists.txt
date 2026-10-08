# Copyright (c) 2026 SPHARX. All Rights Reserved. "From data intelligence emerges."
# daemon 公共库构建文件 - 最小兼容层
#
# 本模块现在作为 commons 统一基础库的兼容层，
# 提供向后兼容的 API，实际实现来自 commons。
#
# 保留的 daemon 特有模块：
# - svc_model_defaults.c: model.yaml 默认模型提取（A2-1 生态面策略件）
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

# 2026-08-27 目录原子化：src/ 按功能域组织为 8 个子目录
#   svc/ 服务框架 · auth/ 认证中间件 · discovery/ 服务发现
#   security/ daemon 安全 · daemon/ daemon 引导与生命周期 · event/ 事件循环
set(SVC_COMMON_SOURCES
    # 0c A 类直搬：svc 生命周期机制族迁 commons/utils/svc（0.1.19 S74 层界
    # 错置回位，svc_common.h 权威头同域归位），符号经 airy_common PUBLIC 提供。
    src/svc/svc_model_defaults.c  # A2-1: model.yaml global 段默认模型提取（llm_d/gateway_d 共用）
    # 0c A 类直搬：jsonrpc_helpers/method_dispatcher/param_validator
    # 权威实现迁 commons/utils/ipc，符号经 airy_common 提供（daemons 留 re-export 头）
    # 0c A 类直搬：跨进程服务发现全家族迁 commons/utils/sd（g14 补完 P0.17
    # 阶段 4 半程迁移），符号经 airy_common PUBLIC 链接提供。
    # 0c SSoT 收敛：熔断器权威实现迁 commons/utils/ipc（src/circuit_breaker.c），
    # 符号经 airy_common PUBLIC 链接提供（include/circuit_breaker.h 兼容头已删，
    # 裸引经 PUBLIC include 路径直接解析 commons 权威）。
    # 0c A 类直搬：unified_metrics/alert_manager/jsonrpc_helpers/method_dispatcher/
    # param_validator/log_sanitizer/api_recovery/airy_event_loop 家族权威实现均迁
    # commons（utils/observability|ipc|security|resource|sync），daemons 留 re-export 头。
    src/auth/svc_auth.c
    src/auth/svc_auth_jwt.c       # 认证中间件-JWT 域：生命周期/令牌生成/刷新（从 svc_auth_jwt.c 拆分）
    src/auth/svc_auth_jwt_crypto.c # 认证中间件-JWT 域：HMAC 实现/Base64 编解码原语（从 svc_auth_jwt.c 拆分）
    src/auth/svc_auth_jwt_verify.c # 认证中间件-JWT 域：令牌验证/签名校验（从 svc_auth_jwt.c 拆分）
    src/auth/svc_auth_apikey.c    # 认证中间件-API Key 域
    src/auth/svc_auth_ratelimit.c # 认证中间件-限流域
    src/security/daemon_security.c     # daemon 安全集成（初始化/消毒域）
    src/security/daemon_security_acl.c       # Phase 2.3a: daemon 安全-ACL 权限域（从 daemon_security.c 拆分）
    src/security/daemon_security_signature.c # Phase 2.3a: daemon 安全-包签名验证域（从 daemon_security.c 拆分）
    src/security/daemon_security_vault.c     # Phase 2.3a: daemon 安全-凭据/审计域（从 daemon_security.c 拆分）
    # 0c A 类直搬：platform_compat 实现迁 commons/platform（src/platform_compat.c），
    # 声明平台扩展头 daemon_platform_ext.h 亦迁 commons，daemons 留 re-export。
    src/daemon/daemon_event_driver.c # 统一事件驱动框架
    # 0c IRON-6 归位：工作线程池实现迁 commons/utils/sync（src/thread_pool.c），
    # 符号经 airy_common PUBLIC 链接提供，daemons 仅留 include/thread_pool.h 兼容头。
    # g14b 补全：daemon_bootstrap_sd.c 迁 commons/utils/sd（daemon_bootstrap_sd.h
    # 公开头已在该处），实现随之归位，符号经 airy_common PUBLIC 链接提供。
    src/daemon/daemon_cupolas_bootstrap.c # P3.14 ACC-DT15: daemon 统一 cupolas 安全穹顶引导
    src/daemon/daemon_rpc_client.c       # Phase 3: gateway→daemon thin IPC client (Unix socket JSON-RPC)
    src/daemon/daemon_heapstore_bootstrap.c # KER-05~07: daemon 统一 heapstore 运行时数据存储引导
    src/daemon/daemon_ipc_ops_bootstrap.c # ARC-04: 注入 atoms 侧 IPC/RPC/SD ops 抽象表
    src/daemon/daemon_dep.c         # 0.1.19 §7.2: 硬依赖声明 + 启动期探测 + 降级态显式上报（governance 面）
    src/daemon/daemon_boot.c        # 0.1.19 §79: daemon 启动机制（十二户 main.c 装配骨架收敛）
    src/daemon/daemon_cfg_file.c    # 0.1.19 t44: config_load/ep_load 七副本消解的文件读取与端点段解析机制
)

# 创建静态库
add_library(svc_common STATIC ${SVC_COMMON_SOURCES})

# 链接 commons 库
target_link_libraries(svc_common PUBLIC
    airy_common
    airy_compile_defs
)

# KER-05~07: heapstore 运行时数据存储（daemon_heapstore_bootstrap 使用）。
# heapstore 仅依赖 commons + threads，无循环依赖（与 atoms/corekern 同模式）。
# 注：CMake 支持前向目标引用，daemons 先于 heapstore 配置但 target_link_libraries
# 在生成阶段解析，与 atoms/corekern 引用 airy_heapstore 同模式。
# §251 债务①塌缩：模块外公开头直接消费面恰为 daemon_heapstore_bootstrap.c
# 一个 TU（svc_common 自有源）；外围经包装头 daemon_heapstore_bootstrap.h
# （零类型传播）或 airy_heapstore_ops 抽象表接入。PRIVATE 降级后链接需求经
# $<LINK_ONLY> 照常落各 daemon 链接行；include 仅本库编译需要，自取 PRIVATE，
# 不再向全部 svc_common 消费方扩散 heapstore 头面。
if(BUILD_HEAPSTORE)
    target_link_libraries(svc_common PRIVATE airy_heapstore)
    target_include_directories(svc_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../heapstore/include)
    # ARC-04: daemon_heapstore_bootstrap 注入 atoms 侧持久化 ops 抽象表。
    # ops 存储（are_ops_set/get_heapstore）在独立小库 airy_syscall_ops 中，
    # 而非 airy_syscall——否则 svc_common → airy_syscall → airy_coreloopthree
    # → svc_common 构成静态库循环依赖。PRIVATE 链接保证最终可执行文件
    # 链接行含该归档；接口 include 由 airy_syscall_ops 自身提供。
    target_link_libraries(svc_common PRIVATE airy_syscall_ops)
endif()

# ARC-04: daemon_ipc_ops_bootstrap 注入 atoms 侧 IPC/RPC/SD ops 抽象表。
# ops 存储（are_ops_set/get_ipc）在独立小库 airy_ipc_ops 中，而非 svc_common
# 自身——否则 atoms 侧（coreloopthree IPC 适配器）消费 are_ops_get_ipc 时需
# 反向链接 svc_common，构成 AIRY-02/IRON-6 违规。PRIVATE 链接保证最终可执行
# 文件链接行含该归档；接口 include（airy_ipc_ops.h）由 coreloopthree/include
# 提供（见本文件 PUBLIC include 第二段）。
target_link_libraries(svc_common PRIVATE airy_ipc_ops)

# WS-8 8.4.1：daemon_boot 进程骨架调用 corekern airy_init() 完成进程原语
# 初始化，该归档依赖由 svc_common 显式 PRIVATE 声明（M5-3 消解 daemon_l1_server
# 灰度滞留件后改直链，闭包与历史形态等价：airy_core 归档仍落各 daemon 链接行
# svc_common 之后）。PRIVATE 保证 corekern 头与编译定义不向 daemon 消费方传播。
target_link_libraries(svc_common PRIVATE airy_core)

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
# 若 daemons/common/include 在前，atoms 源代码 #include 的 daemon 框架重导出头
# 会先命中 daemons 侧同名头（虽然内部经相对路径到达 commons，但仍构成编译期
# atoms→daemons 反向依赖）。将 daemons/common/include 移至 commons 路径之后，
# 确保 atoms 直接解析 commons 权威版本，完全消除 IRON-6 违规。
# 注意：daemons/common/include 中 platform.h / compat.h 两个重导出兼容头
# 已随 0.1.19 施工线⑦拆除（消费者一律经 -I 直解 commons 权威版）；其余
# 头文件均为 daemons 框架自有头，daemons 自己的源代码仍经此路径解析。
target_include_directories(svc_common PUBLIC
    # commons 权威路径（优先）
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/compat>
    $<BUILD_INTERFACE:${COMMONS_DIR}>
    $<BUILD_INTERFACE:${COMMONS_DIR}/platform/include>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/error>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/logging>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/config_unified>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/memory>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/string>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/sync>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/observability>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/io>
    # 0.1.19 §60/§71: 任务大厅事件磁盘格式 SSoT（hall_event.h），
    # 各 daemon（sched_d/tool_d/agent_d/common）直连 hall_evt_write
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/hall>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/types>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/cache>
    # 新增网络和IPC模块
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/network>
    $<BUILD_INTERFACE:${COMMONS_DIR}/utils/ipc>
    # P0.17 阶段 1: daemon_defaults.h 重导出 airy_defaults.h（位于 commons/include）
    $<BUILD_INTERFACE:${COMMONS_DIR}/include>
    # daemons/common/include 兼容层路径（放最后，避免 atoms 误命中重导出兼容头）
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/agentrt>
)

# B16-S0 传播面收敛（V16.13①）：src/ 子目录是私有实现细节，仅供 svc_common
# 自身裸名 include 解析（internal 头消费全在本模块内），严禁 PUBLIC
# 传播；下游一律走显式 target_link_libraries。src/event 为空目录不挂。
# src/svc 已随 S74 迁移清空（仅余 svc_model_defaults.c，自带目录解析）。
# src/ipc 已随 0.1.19 t213 IPC Bus 退役整体删除（总线/helper/HTTP 客户端族）。
target_include_directories(svc_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth
    ${CMAKE_CURRENT_SOURCE_DIR}/src/security
    ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon
)

target_include_directories(svc_common PUBLIC
    $<BUILD_INTERFACE:${ATOMS_DIR}/coreloopthree/include>
    # provider.h 消费通道（memory 公开头收口于 include/）
    $<BUILD_INTERFACE:${ATOMS_DIR}/memory/include>
    # W18: taskflow_advanced.h 被 loop.h 包含（DAG 工作流集成），
    # orchestrator.c 包含 loop.h 因此间接依赖 taskflow 类型定义
    $<BUILD_INTERFACE:${ATOMS_DIR}/taskflow/include>
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
if(AIRY_HAS_YAML)
    target_include_directories(svc_common PRIVATE ${YAML_INCLUDE_DIRS})
    target_link_libraries(svc_common PUBLIC ${YAML_LIBRARIES})
    # svc_model_defaults.c 依赖 yaml.h，仅在本库内使用（HAVE_YAML 与 llm_d 保持一致）
    target_compile_definitions(svc_common PRIVATE HAVE_YAML=1)
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
# P25 模式（Windows 排除）：daemon 链测试整目标守卫——svc_common 依赖的
# 测试面在 MSVC 无 mmap/dirent 等 POSIX 运行语义（G1 wave-3 #106 实证
# airy_mman.h C2059 等），覆盖由 POSIX 腿保障；Windows 侧 G2 只跑基础库层
# ctest，daemon 面产品编译先行（G1）。
if(BUILD_TESTS AND NOT WIN32)
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

# M0-L5（0.1.9）：不再安装兼容层头文件到 include/agentrt。
# 实测：include/ 下 33 头多为 IRON-6 迁移后的 re-export 兼容头（内容仅为
# #include "../../../commons/..." 相对引用，安装位置无法解析——commons 权威头
# 未随装），daemon 框架自有头亦仅源码树内消费；全仓（agentrt/tools/products）
# 零消费者使用该安装面。继续安装只会形成误导性的"第二公共 API 面"。
# 权威 API 头由各库自装（corekern/coreloopthree/commons 等）；daemon 框架对外
# 安装面设计并入 0d/0.2.x 统一规划。
