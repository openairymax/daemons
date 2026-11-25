// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file semantic_cache.c
 * @brief Accounting domain: cross-process semantic cache (L1) over the mem_d
 *        daemon's cache_get / cache_put methods (A-IPC over Unix socket).
 *
 * 缓存是可选加速层：任何失败（daemon 未启动、超时、协议错误、g_cache
 * 未初始化）一律按"未命中/未写入"处理，主流程继续直连上游 LLM，仅以
 * DEBUG 记录，避免 mem_d 缺席时每次请求刷屏。
 *
 * 缓存命名空间（N-6）：键除 text/model 外还带作用域串 cache_ns，避免换版
 * 模型、chat 模板或采样参数变更后旧结果被新请求命中。v1 作用域 =
 * AIRY_SEMCACHE_NS（契约版本）+ 调用方声明的采样参数指纹：
 *   - 模型维度由 mem_d 既有键派生（SHA-256("text|model_id")）与 L1 同模型
 *     过滤承担，不重复拼入作用域；
 *   - chat 模板名在 llm_d 不可见（由 provider 侧渲染），v1 未纳入模板维度；
 *     模板或参数契约变化时把 AIRY_SEMCACHE_NS 版本串 +1，旧条目自然失效；
 *   - 模型级默认采样参数（model.yaml）不在 v1 指纹内，其变更同样靠版本串
 *     +1 或 TTL 收敛。
 */

#include "semantic_cache.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

/* 语义缓存（mem_d）RPC 超时：缓存是可选加速层，本地 socket 往返毫秒级，
 * 此处仅需给出上界以保证 mem_d 繁忙时不拖慢主链路（13-semantic-cache
 * §3.5：缓存引擎异常必须降级为直连 LLM）。 */
#define LLM_MEM_CACHE_TIMEOUT_MS 2000

/* 命名空间契约版本与作用域串上限（mem_d 对超长 cache_ns 按缺省处理，
 * 故这里必须保证拼装结果不超限）。 */
#define AIRY_SEMCACHE_NS "semcache/v1"
#define LLM_SEMCACHE_NS_MAX 64

static const char *mem_sock_path(void)
{
    const char *sock = airy_runtime_dir_socket("mem.sock");
    return (sock && sock[0]) ? sock : NULL;
}

/* 作用域派生唯一来源：契约版本 + 调用方采样参数指纹；指纹缺省时只有版本
 * 段，与未接指纹的老客户端等价。超出上限即截断（指纹格式定长，不会把两个
 * 不同指纹截成同一串）。 */
static void semcache_ns(const char *ns_hint, char out[LLM_SEMCACHE_NS_MAX + 1])
{
    if (ns_hint && ns_hint[0])
        snprintf(out, LLM_SEMCACHE_NS_MAX + 1, "%s|%s", AIRY_SEMCACHE_NS, ns_hint);
    else
        AIRY_STRNCPY_TERM(out, AIRY_SEMCACHE_NS, LLM_SEMCACHE_NS_MAX + 1);
}

int llm_semantic_cache_fetch(const char *text, const char *model, const char *ns_hint,
                             char **out_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !out_json) {
        return 0;
    }
    *out_json = NULL;

    char cache_ns[LLM_SEMCACHE_NS_MAX + 1];
    semcache_ns(ns_hint, cache_ns);

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return 0;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "model_id", model);
    cJSON_AddStringToObject(req, "cache_ns", cache_ns);
    char *params = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!params) {
        return 0;
    }

    char *result = NULL;
    int rc = daemon_rpc_call(sock, "cache_get", params, &result, LLM_MEM_CACHE_TIMEOUT_MS);
    AIRY_FREE(params);
    if (rc != AIRY_SUCCESS || !result) {
        SVC_LOG_DEBUG("mem.cache_get unavailable (rc=%d) — semantic cache bypassed", rc);
        AIRY_FREE(result);
        return 0;
    }

    int hit = 0;
    cJSON *root = cJSON_Parse(result);
    AIRY_FREE(result);
    if (!root) {
        return 0;
    }

    const cJSON *hit_item = cJSON_GetObjectItem(root, "hit");
    const cJSON *body_item = cJSON_GetObjectItem(root, "response");
    if (cJSON_IsBool(hit_item) && cJSON_IsTrue(hit_item) && cJSON_IsString(body_item) &&
        body_item->valuestring && body_item->valuestring[0]) {
        *out_json = AIRY_STRDUP(body_item->valuestring);
        hit = (*out_json != NULL);
    }
    cJSON_Delete(root);
    return hit;
}

void llm_semantic_cache_save(const char *text, const char *model, const char *ns_hint,
                             const char *resp_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !resp_json) {
        return;
    }

    char cache_ns[LLM_SEMCACHE_NS_MAX + 1];
    semcache_ns(ns_hint, cache_ns);

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "response", resp_json);
    cJSON_AddStringToObject(req, "model_id", model);
    cJSON_AddStringToObject(req, "cache_ns", cache_ns);
    /* B5-3：仅在调用方按请求级声明（cacheable）通过准入门禁后才会到达此处，
     * 故显式声明可缓存。 */
    cJSON_AddBoolToObject(req, "cacheable", 1);
    char *params = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!params) {
        return;
    }

    char *result = NULL;
    int rc = daemon_rpc_call(sock, "cache_put", params, &result, LLM_MEM_CACHE_TIMEOUT_MS);
    AIRY_FREE(params);
    AIRY_FREE(result);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_DEBUG("mem.cache_put unavailable (rc=%d) — semantic cache not updated", rc);
    }
}
