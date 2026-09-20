/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file adapter_table.c
 * @brief 适配器装配 SSoT：注册新厂商适配的唯一改动点（B16-S3 c7）。
 *
 * core/registry.c 只经 provider_adapter_lookup() 查表取 ops，不持有任何
 * 厂商符号与厂商名分支。注册新适配 = 本表尾加一行 + 各 CMakeLists 登记
 * 源文件。未命中回落 OpenAI 兼容适配（对齐 LiteLLM 的 openai_like 模
 * 式）：glm / qwen / moonshot / siliconflow / spark / minimax 及自定义名
 * 仅需 model.yaml 提供 base_url + api_key 即以统一 OpenAI Chat
 * Completions 协议接入，无需新适配器实现。
 */

#include "core/adapter.h"

#include "svc_logger.h"

#include <string.h>

extern const provider_adapter_t openai_ops;
extern const provider_adapter_t anthropic_ops;
extern const provider_adapter_t deepseek_ops;
extern const provider_adapter_t google_ops;
extern const provider_adapter_t local_ops;

#define ADAPTER_TABLE_SIZE (sizeof(g_adapter_table) / sizeof(g_adapter_table[0]))

static const provider_adapter_t *const g_adapter_table[] = {
    &openai_ops, &anthropic_ops, &deepseek_ops, &google_ops, &local_ops,
};

const provider_adapter_t *provider_adapter_lookup(const char *name)
{
    if (name) {
        for (size_t i = 0; i < ADAPTER_TABLE_SIZE; ++i) {
            if (strcmp(name, g_adapter_table[i]->name) == 0)
                return g_adapter_table[i];
        }
        SVC_LOG_DEBUG("Provider '%s' falls back to OpenAI-compatible adapter "
                      "(custom base_url)",
                      name);
    }
    return &openai_ops;
}
