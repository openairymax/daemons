# notify_d 模块装配清单（五件套④，手写维护：codegen=false 户；
# 域组织：bootstrap 启动域 / service 核心域 / net 协议域 / hook 面域
# （R7 并户：原 hook_d 独立守护进程吸收为第二监听面））
set(NOTIFY_D_SOURCES
    src/main.c
    src/svc.c
    src/bootstrap/nf_boot.c
    src/service/notify_service.c
    src/net/net.c
    src/hook/hook_svc.c
    src/hook/hook_rpc.c
)
