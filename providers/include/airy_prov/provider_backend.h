/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file provider_backend.h
 * @brief GPU 后端就绪判定契约（0.1.21 B-2）
 *
 * B-0 的本机终论为 R3（无加速器）：B-2 不实现推理后端，只保留「能接、
 * 能建请求、能归错」的适配器骨架，并把「声明了加速器却没有后端地址」
 * 这一状态变成可判读的错误码，而不是等到 HTTP 连接超时（最长数十秒）
 * 才以一个含混的传输错误返回。
 *
 * 判定只读配置，不做任何 I/O：扫描在 registry 装配时执行一次，请求路径
 * 只读结果，因此未配置时的失败时延上界由 PROVIDER_BACKEND_FAILFAST_MS
 * 约束（实际为本地分支，微秒级）。
 */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_BACKEND_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_BACKEND_H

#include "provider_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 未配置后端的失败时延预算（毫秒）；请求须在此预算内返回错误码。 */
#define PROVIDER_BACKEND_FAILFAST_MS 5000

/** 后端类别：只描述线格式所属族，不写具体产品名（禁冒名）。 */
typedef enum {
    PROVIDER_BACKEND_KIND_NONE = 0,   /**< 未声明加速器：沿用既有默认端点 */
    PROVIDER_BACKEND_KIND_OPENAI = 1, /**< OpenAI 兼容端点（本地推理服务） */
} provider_backend_kind_t;

/** 装配期计算出的后端就绪状态。 */
typedef struct {
    /** 1 = 可发请求；0 = 未配置（请求须以 -418 快速失败）。 */
    int ready;
    /** 线格式族；未声明加速器时为 NONE。 */
    provider_backend_kind_t kind;
    /** ready=0 时的可判读原因（直接进日志与错误回包）。 */
    char reason[160];
} provider_backend_t;

/**
 * @brief 按 provider 配置扫描后端就绪状态（纯函数，无 I/O）。
 *
 * 判定规则：
 *  - 未声明能力位、或加速器为 AIRY_ACCEL_NONE（未知）——按旧路径可用；
 *  - 声明了具体加速器且给出 api_base——OpenAI 兼容后端就绪；
 *  - 声明了具体加速器但 api_base 缺失——未配置，reason 写明缺项。
 *
 * @param pcfg provider 配置（可空；空视为配置缺失）。
 * @param out  输出状态（不可空；内部清零后填充）。
 */
void prov_be_scan(const provider_config_t *pcfg, provider_backend_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_BACKEND_H */
