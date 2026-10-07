# notify_d 模块装配清单（五件套④，手写维护：codegen=false 户；
# R7 并户追加 hook 面：生命周期域 hook_svc.c + 方法域 hook_rpc.c）
set(NOTIFY_D_SOURCES
    src/main.c
    src/nf_boot.c
    src/svc.c
    src/net.c
    src/notify_service.c
    src/hook_svc.c
    src/hook_rpc.c
)
