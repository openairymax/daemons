/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file adapter_table.c
 * @brief 适配器装配 SSoT：注册新线格式适配的唯一改动点（B16-S3 c7）。
 *
 * core/registry.c 只经 provider_adapter_lookup() 按线格式名（model.yaml 的
 * api_format）查表取 ops，不持有任何厂商符号与厂商名分支。注册新适配 =
 * 本表尾加一行 + 各 CMakeLists 登记源文件。未命中回落 chat 并记 WARN：
 * 任何厂商只要 model.yaml 提供 api_format / base_url / api_key 即可接入，
 * 同线格式的多家厂商共用同一适配器，无需新增实现。
 */

#include "core/adapter.h"

#include "svc_logger.h"

#include <string.h>

extern const provider_adapter_t chat_ops;
extern const provider_adapter_t messages_ops;
extern const provider_adapter_t local_ops;

#define ADAPTER_TABLE_SIZE (sizeof(g_adapter_table) / sizeof(g_adapter_table[0]))

static const provider_adapter_t *const g_adapter_table[] = {
    &chat_ops, &messages_ops, &local_ops,
};

const provider_adapter_t *provider_adapter_lookup(const char *fmt)
{
    if (fmt && fmt[0]) {
        for (size_t i = 0; i < ADAPTER_TABLE_SIZE; ++i) {
            if (strcmp(fmt, g_adapter_table[i]->name) == 0)
                return g_adapter_table[i];
        }
        SVC_LOG_WARN("C-L02: ADAPTER: unknown api_format=%s, falling back to chat", fmt);
    }
    return &chat_ops;
}
