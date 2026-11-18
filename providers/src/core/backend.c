/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file backend.c
 * @brief GPU 后端就绪判定（0.1.21 B-2 机制面）
 *
 * 只读 provider 配置，不探测端口、不发请求。B-0 终论为 R3 时，这里把
 * 「有加速器声明、无后端地址」与「后端可用」分开，使请求路径能在任何
 * 网络 I/O 之前以 AIRY_ERR_LLM_BACKEND_UNCONFIGURED 返回。
 */

#include "airy_prov/provider_backend.h"

#include <stdio.h>

/* 是否显式声明了具体加速器；NONE 表示未知，不能当无 GPU，也不能当已配置。 */
static int be_has_accel(const provider_config_t *pcfg)
{
    return pcfg->hw.declared && pcfg->hw.accelerator != AIRY_ACCEL_NONE;
}

void prov_be_scan(const provider_config_t *pcfg, provider_backend_t *out)
{
    if (!out)
        return;

    out->ready = 1;
    out->kind = PROVIDER_BACKEND_KIND_NONE;
    out->reason[0] = '\0';

    if (!pcfg) {
        out->ready = 0;
        snprintf(out->reason, sizeof(out->reason), "provider 配置缺失，后端未配置");
        return;
    }
    if (!be_has_accel(pcfg))
        return;

    out->kind = PROVIDER_BACKEND_KIND_STDAPI;
    if (!pcfg->api_base || pcfg->api_base[0] == '\0') {
        out->ready = 0;
        snprintf(out->reason, sizeof(out->reason),
                 "加速器已声明（kind=%d）但 api_base 缺失，后端未配置",
                 (int)pcfg->hw.accelerator);
    }
}
