/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_retry_class.h: Provider 域可重试类别（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_CLASS_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_CLASS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 可重试类别（策略面）：机制件 provider_retry.c 只认"哪些 libcurl 故障可重发"，
 * 是否收窄由端点自身声明。本地慢推理是预期行为而非瞬时故障，超时重发只会
 * 空转整轮预算，故本地端点声明 CONNECT_ONLY——仅连接建立失败可重发。 */
typedef enum {
    PROVIDER_RETRY_DEFAULT = 0,      /* 全类别瞬时故障可重试（云端端点默认） */
    PROVIDER_RETRY_CONNECT_ONLY = 1, /* 仅连接建立失败可重试；超时/收发中断即终局 */
} provider_retry_class_t;

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_RETRY_CLASS_H */
