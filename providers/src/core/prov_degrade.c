// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file prov_degrade.c
 * @brief 部署位阶判定实现（0.1.21 B-4）：GPU → CPU → 远端降级阶梯。
 *
 * 能力位与存活判据分离：本件只回答"部署落在阶梯哪一级"，不回答"端点
 * 能否服务"；可达性由 ep_health 探测、熔断由 prov_breaker 记账。三者
 * 共用同一端点事实，但各自结论不互相推导。
 */

#include "airy_prov/prov_degrade.h"

#include "airy_prov/provider_base.h"

#include <string.h>

int prov_host_local(const char *api_base)
{
    static const char *const kLocal[] = {"127.0.0.1", "localhost", "[::1]"};

    if (!api_base || api_base[0] == '\0')
        return 0;

    /* 带 scheme 时只看 host 段；裸 host 原样判定。 */
    const char *host = strstr(api_base, "://");
    host = host ? host + 3 : api_base;

    for (size_t i = 0; i < sizeof(kLocal) / sizeof(kLocal[0]); ++i) {
        size_t n = strlen(kLocal[i]);
        if (strncmp(host, kLocal[i], n) != 0)
            continue;
        char c = host[n];
        if (c == '\0' || c == ':' || c == '/' || c == '?')
            return 1;
    }
    return 0;
}

prov_deg_t prov_deg_rank(const provider_t *prov)
{
    if (!prov || !prov->name)
        return PROV_DEG_NONE;

    const provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    if (!base || base->api_base[0] == '\0')
        return PROV_DEG_NONE;

    /* 远端优先按位置判：数据出域与网络依赖不因远端有 GPU 而消失。 */
    if (!prov_host_local(base->api_base))
        return PROV_DEG_REMOTE;

    if (prov->hw.declared && prov->hw.accelerator != AIRY_ACCEL_NONE)
        return PROV_DEG_GPU;
    return PROV_DEG_CPU;
}

const char *prov_deg_text(prov_deg_t rank)
{
    switch (rank) {
    case PROV_DEG_GPU:
        return "gpu";
    case PROV_DEG_CPU:
        return "degraded:local_cpu";
    case PROV_DEG_REMOTE:
        return "degraded:remote";
    case PROV_DEG_NONE:
        return "degraded:unknown";
    default:
        return "degraded:unknown";
    }
}
