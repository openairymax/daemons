/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file providers_internal.h
 * @brief llm_d providers 域私有：协议适配器原子件的单入口契约头。
 *
 * 三家协议适配器（adapters/openai / anthropic / local）共同消费的契约
 * 交集：适配层契约（core/adapter.h）+ 请求装配/响应解析信封
 * （core/provider_envelope.h）。新增厂商适配器只写本单入口，不各自
 * 直持两条具体边；core 域内部件（registry / adapter_table / driver）
 * 不在该契约面上，仍直接消费具体头。跨域禁止 include 本头。
 */

#ifndef AIRY_RT_LLM_PROVIDERS_INTERNAL_H
#define AIRY_RT_LLM_PROVIDERS_INTERNAL_H

#include "core/adapter.h"
#include "core/provider_envelope.h"

#endif /* AIRY_RT_LLM_PROVIDERS_INTERNAL_H */
