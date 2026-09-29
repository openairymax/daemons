// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file think_rpc.c
 * @brief think.* JSON-RPC 方法实现域（think.process/orchestrate/get_stats/
 *        health_check 业务逻辑 + lang 与 review 转调薄壳）。
 */

#include "airy_memory.h"
#include "error.h"
#include "lang_svc.h"
#include "review_svc.h"
#include "svc_think_d.h"
#include "daemon_main.h"
#include "think_d_internal.h"

#include <time.h>

static void handle_process(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *prompt = cJSON_GetObjectItem(params, "prompt");
    if (!prompt || !cJSON_IsString(prompt) || !prompt->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing prompt string", id);
        return;
    }

    /* GCCP 两段式交互第二段（P-A）：可选 gccp_answers（用户答案 JSON），
     * 缺省/空串视为第一段（无答案），引擎可能返回问题集挂起。 */
    const char *gccp_answers = NULL;
    cJSON *answers = cJSON_GetObjectItem(params, "gccp_answers");
    if (cJSON_IsString(answers) && answers->valuestring && answers->valuestring[0])
        gccp_answers = answers->valuestring;

    /* GCCP 会话隔离：客户端会话标识，交互状态按此维度隔离，
     * 杜绝多客户端并发串台。缺省/空串使用 "default" 兜底。 */
    const char *session_id = NULL;
    cJSON *sid = cJSON_GetObjectItem(params, "session_id");
    if (cJSON_IsString(sid) && sid->valuestring && sid->valuestring[0])
        session_id = sid->valuestring;

    think_process_result_t res = {0};
    int ret = think_service_process(g_svc, session_id, prompt->valuestring, gccp_answers, &res);
    if (ret != AIRY_SUCCESS || !res.json) {
        /* 错误码细节保留：think_service_process 失败时已在 res.json 内置
         * 含 err_code 的诊断 JSON（err_code/feedback/stats），优先原样返回，
         * 客户端可据此区分真实失败与可降级路径；仅当 JSON 缺失时才回退
         * 通用 -32603。 */
        if (res.json) {
            cJSON *err_obj = cJSON_Parse(res.json);
            if (err_obj) {
                JSONRPC_SEND_SUCCESS(client_fd, err_obj, id);
                think_result_free(&res);
                return;
            }
        }
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Think process failed", id);
        think_result_free(&res);
        return;
    }

    cJSON *res_obj = cJSON_CreateString(res.json);
    think_result_free(&res);
    if (!res_obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
}

static void handle_get_stats(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;

    char *stats = think_service_stats_json(g_svc);
    if (!stats) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Think stats unavailable", id);
        return;
    }
    cJSON *stats_obj = cJSON_Parse(stats);
    AIRY_FREE(stats);
    if (!stats_obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid stats JSON", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, stats_obj, id);
}

static void handle_health_check(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;

    /* 0.1.19 §7.2 不变式 2/3：健康 = 端到端可达。健康面按请求实时重探依赖，
     * 不用启动快照，避免健康假阳性；required 缺失时 healthy=false 并置
     * degraded=true，同时列出依赖明细。 */
    daemon_dep_t deps;
    sd_helper_t *sdh = g_sdh;
    bool probed = daemon_dep_init(&deps, g_think_deps,
                                  sizeof(g_think_deps) / sizeof(g_think_deps[0])) == AIRY_SUCCESS &&
                  daemon_dep_probe(&deps, sdh) == AIRY_SUCCESS;
    bool service_ready = think_service_ready(g_svc) ? true : false;
    bool healthy = service_ready && probed && daemon_dep_ready(&deps);

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "service", "think_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddBoolToObject(result, "degraded", service_ready && !healthy);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    if (probed && daemon_dep_count(&deps) > 0) {
        cJSON *arr = cJSON_AddArrayToObject(result, "dependencies");
        if (arr) {
            size_t count = daemon_dep_count(&deps);
            for (size_t i = 0; i < count; i++) {
                const char *name = NULL;
                bool required = false;
                bool reachable = false;
                if (daemon_dep_at(&deps, i, &name, &required, &reachable) != AIRY_SUCCESS)
                    continue;
                cJSON *item = cJSON_CreateObject();
                if (!item)
                    continue;
                cJSON_AddStringToObject(item, "name", name ? name : "");
                cJSON_AddBoolToObject(item, "required", required);
                cJSON_AddBoolToObject(item, "reachable", reachable);
                cJSON_AddItemToArray(arr, item);
            }
        }
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* S-5: think.orchestrate — 流程编排执行（orchestrator 管线：
 * 分解→规划→生成→批判→验证→审计→对齐）。入参 input 为自然语言任务；
 * 可选 timeout_ms 覆盖默认超时。 */
static void handle_orchestrate(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *input = cJSON_GetObjectItem(params, "input");
    if (!input || !cJSON_IsString(input) || !input->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing input string", id);
        return;
    }

    uint32_t timeout_ms = 0;
    cJSON *timeout = cJSON_GetObjectItem(params, "timeout_ms");
    if (cJSON_IsNumber(timeout) && timeout->valueint > 0)
        timeout_ms = (uint32_t)timeout->valueint;

    char *json = NULL;
    int ret = think_service_orchestrate(g_svc, input->valuestring, timeout_ms, &json);
    if (ret != 0 || !json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Orchestrate failed", id);
        AIRY_FREE(json);
        return;
    }
    cJSON *res_obj = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!res_obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid orchestrate result", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
}

void m_process(cJSON *params, int id, void *user_data)
{
    handle_process(params, id, *(airy_sock_t *)user_data);
}

void m_orchestrate(cJSON *params, int id, void *user_data)
{
    handle_orchestrate(params, id, *(airy_sock_t *)user_data);
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    handle_get_stats(params, id, *(airy_sock_t *)user_data);
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    handle_health_check(params, id, *(airy_sock_t *)user_data);
}

/* lang 与 review 服务面签名同生成器 handler 契约一致
 * （第三参为连接 fd 指针），直通转调零漂移。 */
void m_lang_process(cJSON *params, int id, void *user_data)
{
    lang_svc_process(params, id, user_data);
}

void m_lang_postprocess(cJSON *params, int id, void *user_data)
{
    lang_svc_postprocess(params, id, user_data);
}

void m_lang_stats(cJSON *params, int id, void *user_data)
{
    lang_svc_stats(params, id, user_data);
}

void m_review(cJSON *params, int id, void *user_data)
{
    review_svc_process(params, id, user_data);
}
