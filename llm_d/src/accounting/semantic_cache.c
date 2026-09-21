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
 */

#include "semantic_cache.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

/* 语义缓存（mem_d）RPC 超时：缓存是可选加速层，本地 socket 往返毫秒级，
 * 此处仅需给出上界以保证 mem_d 繁忙时不拖慢主链路（13-semantic-cache
 * §3.5：缓存引擎异常必须降级为直连 LLM）。 */
#define LLM_MEM_CACHE_TIMEOUT_MS 2000

static const char *mem_sock_path(void)
{
    const char *sock = airy_runtime_dir_socket("mem.sock");
    return (sock && sock[0]) ? sock : NULL;
}

int llm_semantic_cache_fetch(const char *text, const char *model, char **out_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !out_json) {
        return 0;
    }
    *out_json = NULL;

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return 0;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "model_id", model);
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

void llm_semantic_cache_save(const char *text, const char *model, const char *resp_json)
{
    const char *sock = mem_sock_path();
    if (!sock || !text || !model || !resp_json) {
        return;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return;
    }
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "response", resp_json);
    cJSON_AddStringToObject(req, "model_id", model);
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
