/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_base.h: Provider 域 base/context 唯一契约（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H

#include "provider_retry_class.h"
#include "llm_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct provider_rate_limiter;

/* 适配器私有上下文的不透明句柄（定义在各适配器 .c 内）。 */
typedef struct provider_ctx provider_ctx_t;

typedef struct {
    /* 端点名（prov_stat 时延样本与自动预算的键；与适配器 ops->name 同名）。 */
    char name[64];
    char api_key[256];
    char api_key_env[128];
    char api_base[512];
    char organization[128];
    /* 端点附加头（如协议版本头），由 model.yaml extra_headers 注入，多条以 ';' 或换行分隔。 */
    char extra_headers[512];
    double timeout_sec;
    /* D2-9（0.1.20）：1 = 预算由实测时延 p99 推导（timeout_sec 此时为 0），
     * 0 = 预算为 timeout_sec 显式值。自动预算只用于本地端点慢推理场景。 */
    int timeout_auto;
    int max_retries;
    provider_retry_class_t retry_class;
    /* C11 能力位：registry 从 provider_config_t 盖入，driver 装配请求时
     * 盖印到 llm_request_config_t.caps，envelope 按位条件注入可选字段。 */
    local_caps_t caps;
    /* D2-9④（0.1.20）：本地端点容量发现（llama.cpp /props，n_ctx 与
     * total_slots 均为 per-slot 语义，R43 四源互证）。props_state:
     * 0=未探测 1=已探测 2=不支持/失败（终态，不重试）；两值 0 = 未知。
     * inflight 为当前在途请求数（sync_atomic 维护，uintptr_t 保 CAS 语义）。 */
    int props_state;
    int n_ctx_slot;
    int total_slots;
    uintptr_t inflight;
} provider_base_ctx_t;

void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name,
                        const char *api_key, const char *api_base, const char *organization,
                        double timeout_sec, int max_retries, const char *default_base);

/* 取 ctx 内嵌的 base（api_base/api_key/timeout_sec 等）。约定：所有 provider
 * 的 ctx 首字段均为 provider_base_ctx_t base。 */
provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H */
