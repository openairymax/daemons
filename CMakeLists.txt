# Copyright (c) 2026 SPHARX. All Rights Reserved. "From data intelligence emerges."
# AgentRT Daemon 模块顶层构建文件

airy_print_info("[DAEMON] Configuring daemon module")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 统一 daemon 二进制输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 构建选项
option(BUILD_TESTS "Build all unit tests" ON)
option(BUILD_COVERAGE "Build with code coverage support" OFF)
option(BUILD_ALL_PLATFORMS "Build for all platforms (cross-compile)" OFF)

# 版本信息
set(DAEMON_VERSION "${PROJECT_VERSION}")

# 打印构建配置
airy_print_info("========================================")
airy_print_info("  AgentRT Daemon Build Configuration")
airy_print_info("========================================")
airy_print_info("  Version:     ${DAEMON_VERSION}")
airy_print_info("  Build Type:  ${CMAKE_BUILD_TYPE}")
airy_print_info("  C Compiler:  ${CMAKE_C_COMPILER}")
airy_print_info("  Build Tests: ${BUILD_TESTS}")
airy_print_info("  Coverage:    ${BUILD_COVERAGE}")
airy_print_info("========================================")

# 包含 common 模块（兼容层）
add_subdirectory(common)

# 包含各个服务模块
add_subdirectory(gateway_d)
add_subdirectory(llm_d)
add_subdirectory(tool_d)
add_subdirectory(monit_d)
add_subdirectory(sched_d)
add_subdirectory(market_d)
add_subdirectory(channel_d)
add_subdirectory(info_d)
add_subdirectory(notify_d)
add_subdirectory(observe_d)
add_subdirectory(hook_d)
add_subdirectory(plugin_d)
# Phase 3：执行体集中化重构 — 新增 daemon（mem.* / agent.* / a2a.* 命名空间）
add_subdirectory(mem_d)
add_subdirectory(agent_d)
add_subdirectory(a2a_d)

# 全局测试支持
if(BUILD_TESTS)
    enable_testing()
    airy_print_ok("  Unit tests:  ENABLED")
    
    # 收集所有测试（GTest 已在根 CMakeLists.txt 中查找）
    if(AIRY_HAS_GTEST)
        airy_print_ok("  GTest:       FOUND")
    else()
        airy_print_warn("  GTest:       NOT FOUND (tests may use custom framework)")
    endif()
else()
    airy_print_warn("  Unit tests:  DISABLED")
endif()

# 打印覆盖率和分析信息
if(BUILD_COVERAGE)
    airy_print_ok("  Coverage:    ENABLED")
    airy_print_info("  Coverage Target: ${COVERAGE_TARGET}%")
    airy_print_info("  Note: Use 'make coverage' to generate coverage reports")
    
    # 查找覆盖率工具
    find_program(LCOV NAMES lcov)
    find_program(GENHTML NAMES genhtml)
    
    if(LCOV AND GENHTML)
        # 添加覆盖率报告目标
        add_custom_target(coverage
            COMMAND ${LCOV} --capture --directory ${CMAKE_BINARY_DIR} --output-file coverage.info
            COMMAND ${LCOV} --remove coverage.info '/usr/*' --output-file coverage.info
            COMMAND ${GENHTML} coverage.info --output-directory ${CMAKE_BINARY_DIR}/coverage_report
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating coverage report..."
        )
        
        # 添加覆盖率清理目标
        add_custom_target(coverage-clean
            COMMAND ${CMAKE_COMMAND} -E remove -f *.gcda *.gcno coverage.info
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage_report
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Cleaning coverage data..."
        )
    else()
        airy_print_warn("  Warning: lcov/genhtml not found - coverage report disabled")
    endif()
endif()

airy_print_info("========================================")

# 打印所有子项目状态
airy_print_info("")
airy_print_info("Sub-modules:")
airy_print_info("  - gateway_d : ${CMAKE_CURRENT_SOURCE_DIR}/gateway_d")
airy_print_info("  - llm_d     : ${CMAKE_CURRENT_SOURCE_DIR}/llm_d")
airy_print_info("  - tool_d    : ${CMAKE_CURRENT_SOURCE_DIR}/tool_d")
airy_print_info("  - monit_d   : ${CMAKE_CURRENT_SOURCE_DIR}/monit_d")
airy_print_info("  - sched_d   : ${CMAKE_CURRENT_SOURCE_DIR}/sched_d")
airy_print_info("  - market_d  : ${CMAKE_CURRENT_SOURCE_DIR}/market_d")
airy_print_info("  - channel_d : ${CMAKE_CURRENT_SOURCE_DIR}/channel_d")
airy_print_info("  - info_d    : ${CMAKE_CURRENT_SOURCE_DIR}/info_d")
airy_print_info("  - notify_d  : ${CMAKE_CURRENT_SOURCE_DIR}/notify_d")
airy_print_info("  - observe_d : ${CMAKE_CURRENT_SOURCE_DIR}/observe_d")
airy_print_info("  - hook_d    : ${CMAKE_CURRENT_SOURCE_DIR}/hook_d")
airy_print_info("  - plugin_d  : ${CMAKE_CURRENT_SOURCE_DIR}/plugin_d")
airy_print_info("  - mem_d     : ${CMAKE_CURRENT_SOURCE_DIR}/mem_d")
airy_print_info("  - agent_d   : ${CMAKE_CURRENT_SOURCE_DIR}/agent_d")
airy_print_info("  - a2a_d     : ${CMAKE_CURRENT_SOURCE_DIR}/a2a_d")
airy_print_info("")

# 安装支持
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/common/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/gateway_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/llm_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tool_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/monit_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/sched_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/market_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/plugin_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/mem_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/agent_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/a2a_d/include/
    DESTINATION include/agentrt
    FILES_MATCHING PATTERN "*.h"
)

# 导出项目配置
if(TARGET svc_common AND TARGET gateway_d AND TARGET llm_d AND TARGET tool_d
   AND TARGET monit_d AND TARGET sched_d AND TARGET market_d
   AND TARGET channel_d AND TARGET info_d AND TARGET notify_d
   AND TARGET observe_d AND TARGET hook_d AND TARGET plugin_d
   AND TARGET mem_d AND TARGET agent_d AND TARGET a2a_d)
    # 注意：仅导出 15 个 daemon 可执行文件（含 Phase 3 新增 mem_d/agent_d/a2a_d）。
    # svc_common 是 STATIC 库且 PUBLIC 链接 airy_common/airy_compile_defs/
    # cupolas/airy_platform_libs，若将其加入 export(TARGETS) 列表，
    # 则要求上述 4 个传递依赖也在同一 export set（它们定义于 commons/cupolas 等
    # 独立 CMakeLists.txt，无法在本文件加入）。daemon 可执行文件对 svc_common
    # 及 service 库均为 PRIVATE 链接，不产生 INTERFACE_LINK_LIBRARIES 传递要求，
    # 因此单独导出可执行文件即可满足 build-tree 消费场景。
    export(TARGETS gateway_d llm_d tool_d
                  monit_d sched_d market_d channel_d
                  info_d notify_d observe_d
                  hook_d plugin_d
                  mem_d agent_d a2a_d
        FILE "${CMAKE_CURRENT_BINARY_DIR}/agentrt-daemon-targets.cmake"
    )
    export(PACKAGE agentrt-daemon)
endif()

# ==================== 生产安装：daemon 二进制 ====================
# cmake --install --prefix $AIRY_HOME → 15 个 daemon 装到 $AIRY_HOME/bin。
# 与 bootstrap AIRY_HOME/bin、agent_build_agents_pypath 的 AIRY_LIB_DIR
# 回退组成完整生产安装（AIRY_HOME 体系，K8s 节点运行时根类比）。
install(TARGETS gateway_d llm_d tool_d
                monit_d sched_d market_d channel_d
                info_d notify_d observe_d
                hook_d plugin_d
                mem_d agent_d a2a_d
    RUNTIME DESTINATION bin
)