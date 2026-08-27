// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_execute.c
 * @brief Tool service 执行域：工具执行（同步/流式）、参数校验与结果缓存。
 *
 * 2026-08-27 域拆分（原 service.c 888 行 → 3 文件）：生命周期/注册表/统计
 * 域见 service.c，builtin 工具注册域见 service_builtin.c。
 */

#include "airy_memory.h"
#include "daemon_security.h"
#include "error.h"
#include "executor.h"
#include "service.h"
#include "svc_logger.h"
#include "tool_service_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Get tool metadata
 */
static tool_metadata_t *get_tool_metadata(tool_service_t *svc, const char *tool_id)
{
    if (!svc || !tool_id) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, tool_id);
    airy_mtx_unlock(&svc->lock);

    return meta;
}

/**
 * @brief Validate tool params
 */
static int validate_tool_params(tool_service_t *svc, tool_metadata_t *meta, const char *tool_id,
                                const char *params_json)
{
    if (!svc || !meta || !tool_id) {
        return AIRY_EINVAL;
    }

    if (svc->validator) {
        int valid = tool_validator_validate(svc->validator, meta, params_json);
        if (!valid) {
            SVC_LOG_WARN("Parameter validation failed for tool: %s", tool_id);
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Get the cached tool result (cache key includes agent_id, avoiding
 *        cross-subject cache that would bypass approval)
 */
static tool_result_t *get_cached_result(tool_service_t *svc, tool_metadata_t *meta,
                                        const char *tool_id, const char *params_json,
                                        const char *agent_id)
{
    if (!svc || !meta || !tool_id || !params_json) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (!meta->cacheable) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *cache_key = tool_cache_key(tool_id, params_json, agent_id);
    if (!cache_key) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *cached = NULL;
    if (tool_cache_get(svc->cache, cache_key, &cached) == 1 && cached) {
        tool_result_t *res = tool_result_from_json(cached);
        AIRY_FREE(cached);
        cached = NULL;
        if (res) {
            SVC_LOG_DEBUG("Cache hit for tool: %s", tool_id);
            AIRY_FREE(cache_key);
            return res;
        }
        SVC_LOG_WARN("Failed to parse cached result for tool: %s", tool_id);
    }

    AIRY_FREE(cache_key);
    cache_key = NULL;
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/**
 * @brief Cache the tool result (cache key includes agent_id, consistent with
 *        the get path)
 */
static void cache_tool_result(tool_service_t *svc, tool_metadata_t *meta, const char *tool_id,
                              const char *params_json, const char *agent_id, tool_result_t *res)
{
    if (!svc || !meta || !tool_id || !params_json || !res || !res->success) {
        return;
    }

    if (!meta->cacheable) {
        return;
    }

    char *cache_key = tool_cache_key(tool_id, params_json, agent_id);
    if (!cache_key) {
        return;
    }

    char *res_json = tool_result_to_json(res);
    if (res_json) {
        tool_cache_put(svc->cache, cache_key, res_json);
        AIRY_FREE(res_json);
        res_json = NULL;
    }

    AIRY_FREE(cache_key);
    cache_key = NULL;
}

/**
 * @brief Execute a tool
 */
static int do_execute_tool(tool_service_t *svc, tool_metadata_t *meta, const char *params_json,
                           const char *agent_id, tool_result_t **out_result)
{
    if (!svc || !meta || !out_result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    tool_result_t *res = NULL;
    int ret = tool_executor_run(svc->executor, meta, params_json, agent_id, &res);

    if (ret != 0) {
        SVC_LOG_ERROR("Tool execution failed, error: %d", ret);
        /* Keep result so the upper layer can read res->error (e.g. the
         * "User denied tool execution" of interactive approval deny/timeout);
         * released by the caller tool_service_execute. */
        *out_result = res;
        return ret;
    }

    *out_result = res;
    return AIRY_OK;
}

int tool_service_execute(tool_service_t *svc, const tool_execute_request_t *req,
                         tool_result_t **out_result)
{
    if (!svc || !req || !out_result) {
        SVC_LOG_ERROR("Invalid parameters to tool_service_execute");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!req->tool_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    tool_metadata_t *meta = get_tool_metadata(svc, req->tool_id);
    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    int valid = validate_tool_params(svc, meta, req->tool_id, req->params_json);
    if (valid <= 0) {
        tool_metadata_free(meta);
        return AIRY_ERROR_TOOL_VALIDATION;
    }

    /* 3. Check the cache.
     * P0 interactive-approval semantics: only subjects already authorized by
     * the static ACL may hit the cache. Unauthorized subjects (needing human
     * decision; allow is a one-time grant) must go through approval on every
     * call — even if previously approved and cached, this call's permission
     * confirmation cannot be skipped. */
    tool_result_t *cached_result = NULL;
    const char *subject = (req->agent_id && req->agent_id[0]) ? req->agent_id : "tool_d";
    if (daemon_check_tool_permission(subject, req->tool_id, "execute") == 0) {
        cached_result = get_cached_result(svc, meta, req->tool_id, req->params_json, req->agent_id);
    }
    if (cached_result) {
        tool_metadata_free(meta);
        *out_result = cached_result;
        svc->exec_total++;
        return AIRY_OK;
    }

    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = do_execute_tool(svc, meta, req->params_json, req->agent_id, &res);
    airy_time_monotonic(&ts1);
    svc->exec_total++;
    svc->exec_ms_total += airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0);
    if (ret != 0) {
        svc->exec_fail++;
        tool_metadata_free(meta);
        meta = NULL;

        *out_result = res;
        return ret;
    }

    cache_tool_result(svc, meta, req->tool_id, req->params_json, req->agent_id, res);

    *out_result = res;
    if (meta) {
        tool_metadata_free(meta);
        meta = NULL;
    }
    return AIRY_OK;
}

int tool_service_execute_stream(tool_service_t *svc, const tool_execute_request_t *req,
                                tool_stream_callback_t callback, void *callback_data,
                                tool_result_t **out_result)
{
    if (!svc || !req || !callback) {
        SVC_LOG_ERROR("Invalid parameters to tool_service_execute_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!req->tool_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, req->tool_id);
    airy_mtx_unlock(&svc->lock);

    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    if (svc->validator) {
        int valid = tool_validator_validate(svc->validator, meta, req->params_json);
        if (!valid) {
            SVC_LOG_WARN("Parameter validation failed for tool: %s", req->tool_id);
            tool_metadata_free(meta);
            return AIRY_ERROR_TOOL_VALIDATION;
        }
    }

    if (!meta->executable || !strstr(meta->executable, "stream")) {

        SVC_LOG_INFO("Tool does not support streaming, using synchronous execution");
    }

    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = tool_executor_run(svc->executor, meta, req->params_json, req->agent_id, &res);
    airy_time_monotonic(&ts1);
    svc->exec_total++;
    svc->exec_ms_total += airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0);

    if (ret == 0 && res) {
        if (callback) {
            if (res->output) {
                callback(res->output, 0, callback_data);
            }
            if (res->error) {
                callback(res->error, 1, callback_data);
            }
        }
        if (out_result) {
            *out_result = res;
        }
    } else {
        if (out_result) {
            *out_result = res;
        }
    }

    if (ret != 0) {
        svc->exec_fail++;
        SVC_LOG_ERROR("Tool stream execution failed: %s, error: %d", req->tool_id, ret);
    }

    tool_metadata_free(meta);
    return ret;
}
