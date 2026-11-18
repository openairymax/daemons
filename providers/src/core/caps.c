// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file caps.c
 * @brief provider 能力位匹配（0.1.21 B-1 机制面）
 *
 * 只回答「该 provider 是否满足要求」，不做注册表遍历（见 registry.c 的
 * provider_select）也不实现探测（B-3）。未知能力位一律按
 * 保守可用处理：不否决、不宣称，由调用方按 EXACT/CONSERVATIVE 决定是否
 * 记录降级原因。
 */

#include "airy_prov/provider_caps.h"

#include <stddef.h>

/* 要求表里是否有任何能力约束；model 只用于成员判定，不算能力。 */
static int sel_has_requirement(const provider_select_t *sel)
{
    return sel->require_accel != AIRY_ACCEL_NONE || sel->min_vram_bytes != 0 ||
           sel->min_context != 0 || sel->need_stream || sel->need_tools;
}

/* 显式值可判：满足返回 1，违反返回 0。未知（-1）不在此判定。 */
static int flag_ok(int actual, int required, int *unknown)
{
    if (!required)
        return 1;
    if (actual < 0) {
        *unknown = 1;
        return 1;
    }
    return actual != 0;
}

int provider_caps_match(const provider_caps_t *caps, const provider_select_t *sel)
{
    int unknown = 0;

    if (!sel)
        return PROVIDER_CAPS_EXACT;
    if (!caps || !caps->declared)
        return sel_has_requirement(sel) ? PROVIDER_CAPS_CONSERVATIVE : PROVIDER_CAPS_EXACT;

    if (sel->require_accel != AIRY_ACCEL_NONE) {
        if (caps->accelerator == AIRY_ACCEL_NONE)
            unknown = 1;
        else if (caps->accelerator != sel->require_accel)
            return PROVIDER_CAPS_MISS;
    }

    if (sel->min_vram_bytes != 0) {
        if (caps->vram_bytes == 0)
            unknown = 1;
        else if (caps->vram_bytes < sel->min_vram_bytes)
            return PROVIDER_CAPS_MISS;
    }

    if (sel->min_context != 0) {
        if (caps->max_context == 0)
            unknown = 1;
        else if (caps->max_context < sel->min_context)
            return PROVIDER_CAPS_MISS;
    }

    if (!flag_ok(caps->supports_stream, sel->need_stream, &unknown))
        return PROVIDER_CAPS_MISS;
    if (!flag_ok(caps->supports_tools, sel->need_tools, &unknown))
        return PROVIDER_CAPS_MISS;

    return unknown ? PROVIDER_CAPS_CONSERVATIVE : PROVIDER_CAPS_EXACT;
}
