# gateway_d 模块装配清单（五件套④，手写维护：codegen=false 户；
# 域组织：service 核心域与 protocol 协议域并入 airy_gateway_service
# 静态库，bootstrap 进程装配域随可执行目标构建（同 llm_d 拆分，
# 服务域重复编译零容忍）；gw_mcpclients.c 为 POSIX-only，WIN32 恒裁剪，
# 由 CMakeLists 在 NOT WIN32 分支以 target_sources 追加。
set(GATEWAY_D_SVC_SOURCES
    src/svc.c
)

set(GATEWAY_D_PROTOCOL_SOURCES
    src/protocol/gateway_jsonpick.c
    src/protocol/gateway_mcp_server.c
    src/protocol/gateway_a2a_handler.c
    src/protocol/gateway_llm_api.c
    src/protocol/gateway_protocol_router.c
)

set(GATEWAY_D_SOURCES
    src/main.c
    src/bootstrap/gw_boot.c
    src/bootstrap/gw_cli.c
    src/bootstrap/gw_acl.c
)
