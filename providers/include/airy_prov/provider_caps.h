/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file provider_caps.h
 * @brief provider 能力位与选路匹配契约（0.1.21 B-1）
 *
 * 抽象层此前按「协议」建模：远端 API 与本地兼容端点只差 api_format，
 * 没有加速器 / 显存 / 上下文 / 流式 / 工具等能力位，选路只能按名字硬编码。
 * 本头把「能力」独立成数据结构，选路按能力匹配，名字只作精确命中键。
 *
 * 三值语义：declared=0 表示未声明或未探测，所有字段未知。未知字段在匹配时
 * 走「保守可用」——不因缺证据否决候选，但也不宣称精确命中；declared=1 时
 * supports_stream / supports_tools 取 -1 仍表示未知，0/1 才是显式否决/支持。
 */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_CAPS_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_CAPS_H

#include <stdint.h>

#include "platform_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单 provider 的静态能力位。 */
typedef struct {
    /* 0 = 未声明/未探测（全部未知）；1 = 以下字段有显式来源。 */
    int declared;
    /* 加速器种类（platform_gpu.h 的 airy_accel_kind_t 值）；
     * AIRY_ACCEL_NONE 表示未知，不得当作「无 GPU」。 */
    airy_accel_kind_t accelerator;
    /* 可用显存字节数；0 = 未知（Intel iGPU 共享内存同样记 0）。 */
    uint64_t vram_bytes;
    /* 上下文上限（token）；0 = 未知。 */
    uint32_t max_context;
    /* -1 未知 / 0 不支持 / 1 支持。 */
    int supports_stream;
    int supports_tools;
} provider_caps_t;

/** 一次选路的能力要求；零值表示不限制。 */
typedef struct {
    /* 限定模型名（可空）；只匹配声明了该模型的 provider。 */
    const char *model;
    /* 要求加速器种类；AIRY_ACCEL_NONE = 不限。 */
    airy_accel_kind_t require_accel;
    /* 显存下限（字节）；0 = 不限。 */
    uint64_t min_vram_bytes;
    /* 上下文下限（token）；0 = 不限。 */
    uint32_t min_context;
    /* 需要流式 / 工具调用。 */
    int need_stream;
    int need_tools;
} provider_select_t;

/* 匹配三值：不匹配 / 显式满足 / 证据不足的保守可用。 */
#define PROVIDER_CAPS_MISS         0
#define PROVIDER_CAPS_EXACT        1
#define PROVIDER_CAPS_CONSERVATIVE 2

/**
 * @brief 按能力要求评估一个 provider。
 *
 * NULL caps 或 declared=0 时，只要存在任一要求即返回 CONSERVATIVE，否则
 * EXACT；declared=1 时逐字段比较，显式违反返回 MISS。
 *
 * @param caps provider 能力位（可空）
 * @param sel  选路要求（可空 = 不限制）
 * @return PROVIDER_CAPS_MISS / EXACT / CONSERVATIVE
 */
int provider_caps_match(const provider_caps_t *caps, const provider_select_t *sel);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_CAPS_H */
