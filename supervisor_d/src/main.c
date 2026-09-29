// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file main.c
 * @brief supervisor_d 装配壳：decl 装载 → 有参走控制口客户端，无参常驻。
 *
 * 用法：
 *   supervisor_d                 常驻监管（launcher 唯一启动路径：nohup &）
 *   supervisor_d activate <name> 单向激活请求（FAILED 复位运维兜底）
 *   supervisor_d stop            收摊归一（supervisor.shutdown）
 *   supervisor_d status          health_check 摘要
 *   supervisor_d --help
 *
 * 生命周期域在 svc.c（V13.4/V13.5）；CLI 域在 ctrl.c；本文件仅装配。
 */

#include "supervisor_d.h"

#include <string.h>

int main(int argc, char **argv)
{
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (sup_decl_load(&ctx) != 0) {
        sup_log("ERROR", "decl load failed");
        return 1;
    }

    if (argc >= 2)
        return sup_ctrl_client(&ctx, argc, argv);

    int lfd = 0;
    if (sup_svc_prepare(&ctx, &lfd) != 0)
        return 1;
    while (sup_svc_step(&ctx, lfd) == 0) {
    }
    sup_svc_clear(&ctx, lfd);
    return 0;
}
