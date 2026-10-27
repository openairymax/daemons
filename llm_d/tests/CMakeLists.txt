# Copyright (c) 2026 SPHARX. All Rights Reserved.

# 测试代码豁免 BAN 严格合规：测试允许使用裸 malloc/free/memset 等函数
add_compile_definitions(AIRY_COMPLIANCE_IMPL)
# LLM 服务单元测试构建文件 (TeamC)


set(COMMONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../commons)
set(ATOMIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../atoms)
set(SYSCALL_INCLUDE_DIR ${ATOMIC_DIR}/syscall/include)
set(COREKERN_INCLUDE_DIR ${ATOMIC_DIR}/corekern/include)

set(LLM_IMPL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/service_config.c
    # service_config.c 域拆分（2026-08-27）：核心 + 4 个职责模块，须与主目标同步
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/service_config_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/service_config_yaml_models.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/service_config_yaml_providers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/service_config_yaml_pricing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/service_providers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/service_request.c
    # 0.1.19 §3.8.4 A3：调用级链式切换机制（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/service_chain.c
    # 0.1.19 §3.8.4 A4：routing 段解析（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/service_route.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/accounting/metrics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/accounting/semantic_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/response.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/accounting/cost_tracker.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/registry.c
    # provider.c / openai.c 域拆分（2026-08-27）：与主目标 LLM_SERVICE_SOURCES 同步
    # B16-S3：providers 域内重排 core/（机制）+ adapters/（适配）
    # B16-S3 c3：secrets.c（原 provider.c，与主目标 LLM_SERVICE_SOURCES 同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/secrets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/http.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/envelope.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/sse.c
    # B16-S3 c8：退避重试循环壳（与主目标 LLM_SERVICE_SOURCES 同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/retry.c
    # 0.1.20 D2-9：传输超时分类计数与自动预算（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/prov_stat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/toolstream.c
    # B16-S3.3：通用 completion driver（与主目标 LLM_SERVICE_SOURCES 同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/adapter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/adapters/chat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/rate_limit.c
    # 0.1.20 B1-d：上游失败线程局域诊断槽（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/core/prov_diag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/adapters/messages.c
    # B16-S4：deepseek.c / google.c 清理（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/adapters/local.c
    # B16-S3 c7：适配器装配 SSoT（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers/adapters/adapter_table.c
    # P3.16 (ACC-DT17): llm_router 集成 — 路由器实现源码（sec44-D 合一）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router/core/llm_router.c
    # 0.1.19 §3.8.2：端点切换决策件（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router/core/llm_failover.c
    # 0.1.19 §3.8.2：端点健康面（与主目标同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router/core/llm_health.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/router/strategy/router_strategy.c
)

set(LLM_TEST_SOURCES
    test_service.c
    test_token_counter.c
    test_response.c
    test_routing_e2e.c
    test_complexity_routing.c
    bench_routing_latency.c
    test_cost_tracker.c
    # reasoning_content 请求/响应透传（DeepSeek/Kimi 思考模式工具循环硬性要求）
    test_provider_reasoning.c
    # N-3 运行级回归：provider HTTP 状态码 → 错误码分诊（进程内回环 HTTP 桩）
    test_provider_http_dispatch.c
    # P3.16 (ACC-DT17): 路由器集成测试
    test_router_integration.c
    # C-4 (R3): 生成参数唯一解析点（三方优先级）与模型输出上限查表回归
    test_gen_params.c
    # C-4 (D): finish_reason canonical 词汇表归一化与截断判定回归
    test_finish_reason.c
    # 0.1.19 §3.8.2：端点切换决策件（错误码 → 可否换端点）回归
    test_failover.c
    # 0.1.19 §3.8.2：端点健康面（熔断三态 + 半开探测）回归
    test_health.c
    # 0.1.19 §3.8.4 A3：调用级链式切换（off 零变化 / 白名单 / 跳数 / 预算）
    test_chain.c
    # 0.1.19 §3.8.4 A4：routing 段解析（缺失不变 / 叠加 / 限幅）回归
    test_route.c
    # 0.1.20 B1-d：上游失败诊断槽（写读 / 截断 / 覆盖 / 线程隔离）回归
    test_prov_diag.c
    # 0.1.20 C11：请求信封参数段契约（caps 门控 / temperature 哨兵）回归
    test_envelope.c
    # 0.1.20 D2-9：传输观测（超时分类计数 / p99 / 自动预算）回归
    test_prov_stat.c
)

set(LLM_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers
    # B16-S5 预置：五域归位后的头搜索路径（与主目标 airy_llm_service 同步）
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/adapter
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/accounting
    ${COMMONS_DIR}
    ${COMMONS_DIR}/platform/include
    ${COMMONS_DIR}/utils/error
    ${COMMONS_DIR}/utils/logging
    ${COMMONS_DIR}/utils/config_unified
    ${COMMONS_DIR}/utils/memory
    ${COMMONS_DIR}/utils/string
    ${COMMONS_DIR}/utils/sync
    ${COMMONS_DIR}/utils/cache
    ${COMMONS_DIR}/utils/compat
    ${SYSCALL_INCLUDE_DIR}
    ${COREKERN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
)

foreach(TEST_SRC ${LLM_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)

    add_executable(test_llm_${TEST_NAME} ${TEST_SRC} ${LLM_IMPL_SOURCES})

    target_include_directories(test_llm_${TEST_NAME} PRIVATE ${LLM_COMMON_INCLUDES})

    target_link_libraries(test_llm_${TEST_NAME} PRIVATE
        svc_common
        Threads::Threads
    )

    # cJSON found by root CMakeLists.txt (AIRY_HAS_CJSON)
    if(AIRY_HAS_CJSON)
        target_link_libraries(test_llm_${TEST_NAME} PRIVATE cjson)
    endif()

    # CURL found by root CMakeLists.txt (AIRY_HAS_CURL)
    if(AIRY_HAS_CURL)
        target_include_directories(test_llm_${TEST_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(test_llm_${TEST_NAME} PRIVATE ${CURL_LIBRARIES})
    endif()

    target_compile_options(test_llm_${TEST_NAME} PRIVATE -Wall -Wextra -g -Wno-unused-function)

    add_test(NAME llm_d_${TEST_NAME} COMMAND test_llm_${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# 0.1.20 批次 1c：T1 前缀稳定化 + T2 槽位亲和的解析域回归。dispatch.c
# 为装配域源（随主可执行目标构建），此处单独入测并自持 g_service 单例，
# 不进通用源集，避免 18 个既有测试目标被迫解析装配域符号。
add_executable(test_llm_dispatch_parse
    test_dispatch_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rpc/dispatch.c
    ${LLM_IMPL_SOURCES}
)
target_include_directories(test_llm_dispatch_parse PRIVATE ${LLM_COMMON_INCLUDES})
target_link_libraries(test_llm_dispatch_parse PRIVATE
    svc_common
    Threads::Threads
)
if(AIRY_HAS_CJSON)
    target_link_libraries(test_llm_dispatch_parse PRIVATE cjson)
endif()
if(AIRY_HAS_CURL)
    target_include_directories(test_llm_dispatch_parse PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(test_llm_dispatch_parse PRIVATE ${CURL_LIBRARIES})
endif()
target_compile_options(test_llm_dispatch_parse PRIVATE -Wall -Wextra -g -Wno-unused-function)
add_test(NAME llm_d_dispatch_parse COMMAND test_llm_dispatch_parse
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
