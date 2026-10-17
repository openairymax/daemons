# supervisor_d 模块装配清单（五件套④，手写维护：codegen=false 户；
# 与 .manifest modules 对齐，装配对 main.c/svc.c 固定在列首）
set(SUPERVISOR_D_SOURCES
    src/main.c
    src/svc.c
    src/decl/decl.c
    src/proc/proc.c
    src/ctrl/ctrl.c
    src/net/net.c
)
