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
#include "rpc/service.h"
#include "svc_logger.h"
#include "rpc/tool_service_internal.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

/* ---- 请求级拒绝的可诊断化（社区反馈④）----
 * 此前两类请求级拒绝（未知工具 / 参数校验失败）只回通用错误码文案，
 * 模型端无法自我纠错，表现为「调用工具总是出错」。现把诊断文本合成进
 * result 信封（failure_class=RESPOND_TO_MODEL），沿既有透传链（agent 循环
 * role=tool 回填）原样送回模型，模型下一轮即可改参重试或换工具。 */

/* 合成「请求被拒」结果：承载面向模型的诊断文本。 */
static tool_result_t *svc_reject_result(const char *msg)
{
    tool_result_t *res = AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!res)
        return NULL;
    res->success = 0;
    res->exit_code = -1;
    res->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL;
    res->error = AIRY_STRDUP(msg);
    if (!res->error) {
        AIRY_FREE(res);
        return NULL;
    }
    return res;
}

/* 名字相似度：公共前缀长度 + 双向子串命中加成。仅用于建议排序。 */
static int name_score(const char *want, const char *cand)
{
    int s = 0;
    while (want[s] && cand[s] && want[s] == cand[s])
        s++;
    if (s > 0 && (strstr(cand, want) || strstr(want, cand)))
        s += 2;
    return s;
}

#define SUGGEST_KEEP 3

typedef struct {
    const char *want;
    char keep[SUGGEST_KEEP][64];
    int score[SUGGEST_KEEP];
    int n;
} suggest_ctx_t;

/* top-K 插入：新分进榜则整体右移挤尾。 */
static void suggest_insert(suggest_ctx_t *c, const char *id, int sc)
{
    if (c->n == SUGGEST_KEEP && sc <= c->score[SUGGEST_KEEP - 1])
        return;
    int at = (c->n == SUGGEST_KEEP) ? SUGGEST_KEEP - 1 : c->n++;
    while (at > 0 && c->score[at - 1] < sc) {
        c->score[at] = c->score[at - 1];
        snprintf(c->keep[at], sizeof(c->keep[0]), "%s", c->keep[at - 1]);
        at--;
    }
    c->score[at] = sc;
    snprintf(c->keep[at], sizeof(c->keep[0]), "%s", id);
}

static void suggest_feed(const char *id, void *ud)
{
    suggest_ctx_t *c = ud;
    suggest_insert(c, id, name_score(c->want, id));
}

/* 未名建议：取注册表中最相近的至多 3 个工具名进拒绝文本。 */
static void svc_suggest(tool_service_t *svc, const char *want, char *buf, size_t cap)
{
    suggest_ctx_t c = {0};
    c.want = want;
    reg_each_name(svc->registry, suggest_feed, &c);
    if (c.n <= 0) {
        snprintf(buf, cap, "unknown tool '%s'; no similar tool is registered", want);
        return;
    }
    int off = snprintf(buf, cap, "unknown tool '%s'; did you mean", want);
    for (int i = 0; i < c.n && off > 0 && off < (int)cap; i++)
        off += snprintf(buf + off, cap - (size_t)off, "%s '%s'",
                        (i == 0) ? "" : " or", c.keep[i]);
}

/* 参数契约文本：name(type, required?) 列表，随校验失败回传模型。 */
static void param_contract(const tool_metadata_t *meta, char *buf, size_t cap)
{
    if (meta->param_count == 0) {
        snprintf(buf, cap, "none (tool takes no parameters)");
        return;
    }
    size_t off = 0;
    for (size_t i = 0; i < meta->param_count; i++) {
        const tool_param_t *p = &meta->params[i];
        const char *ty = "any";
        cJSON *ps = p->schema ? cJSON_Parse(p->schema) : NULL;
        if (ps) {
            cJSON *t = cJSON_GetObjectItem(ps, "type");
            if (cJSON_IsString(t) && t->valuestring[0])
                ty = t->valuestring;
        }
        int n = snprintf(buf + off, cap - off, "%s%s(%s%s)", (off ? ", " : ""),
                         p->name ? p->name : "?", ty, p->required ? ", required" : "");
        if (ps)
            cJSON_Delete(ps);
        if (n < 0 || (size_t)n >= cap - off)
            break;
        off += (size_t)n;
    }
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
    if (cache_get_string(svc->cache, cache_key, &cached) == 1 && cached) {
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
        cache_put_string(svc->cache, cache_key, res_json);
        AIRY_FREE(res_json);
        res_json = NULL;
    }

    AIRY_FREE(cache_key);
    cache_key = NULL;
}

/**
 * @brief Execute a tool
 *
 * R1-a: 执行经专用隔离池（不再占用 daemon RPC 共享处理池线程）；
 * 队列满背压快失败（AIRY_ERR_BUSY），等待预算耗尽返回 AIRY_ERR_CANCELED
 * 并附带合成取消结果。
 */
static int do_execute_tool(tool_service_t *svc, tool_metadata_t *meta, const char *params_json,
                           const char *agent_id, tool_result_t **out_result)
{
    if (!svc || !meta || !out_result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!svc->exec_pool) {
        return AIRY_ERR_INVALID_PARAM;
    }

    tool_result_t *res = NULL;
    int ret = executor_pool_run(svc->exec_pool, meta, params_json, agent_id, &res);

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

    tool_metadata_t *meta = tool_service_get(svc, req->tool_id);
    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        char diag[256];
        svc_suggest(svc, req->tool_id, diag, sizeof(diag));
        *out_result = svc_reject_result(diag);
        return AIRY_ERR_EXEC_NOT_FOUND;
    }

    int valid = validate_tool_params(svc, meta, req->tool_id, req->params_json);
    if (valid <= 0) {
        char contract[512];
        char diag[640];
        param_contract(meta, contract, sizeof(contract));
        snprintf(diag, sizeof(diag),
                 "parameter validation failed for tool '%s'; declared params: %s; "
                 "fix the arguments and retry",
                 req->tool_id, contract);
        tool_metadata_free(meta);
        *out_result = svc_reject_result(diag);
        return AIRY_ERR_EXEC_VALIDATION;
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
        atomic_fetch_add_explicit(&svc->exec_total, 1, memory_order_relaxed);
        return AIRY_OK;
    }

    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = do_execute_tool(svc, meta, req->params_json, req->agent_id, &res);
    airy_time_monotonic(&ts1);
    atomic_fetch_add_explicit(&svc->exec_total, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&svc->exec_ms_total,
                              (uint64_t)(airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0)),
                              memory_order_relaxed);
    if (ret != 0) {
        atomic_fetch_add_explicit(&svc->exec_fail, 1, memory_order_relaxed);
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
