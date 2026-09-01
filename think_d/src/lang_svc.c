// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file lang_svc.c
 * @brief think.* 命名空间推理语言网关服务面（M1-1c）。
 *
 * CLI 原进程内直连 coreloopthree lang_gateway（输入标准化/输出后处理，
 * 0.1.9 方案 §2.3 判定为"策略直连"）。本模块把 lang_gateway 能力收拢到
 * think_d 服务面，CLI 经 gateway 调用 think.lang_process /
 * think.lang_postprocess / think.lang_stats，满足"认知引擎只对 daemon
 * 服务面暴露；CLI 是引擎壳"的目标态。
 *
 * lang_gateway 库本体（atoms/coreloopthree）不迁移，M3 阶段再物理迁入；
 * 本模块承载其生命周期（懒创建 + 线程安全）。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "jsonrpc_helpers.h"
#include "lang_gateway.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

/* lang_gateway 懒创建后全局持有；airy_mtx 保护并发访问
 * （daemon 事件驱动 concurrent_clients=true，多连接可同时调用） */
static airy_lang_gateway_t *g_lang_gw = NULL;
static airy_mtx_t g_lang_mtx;

int lang_svc_init(void)
{
    if (airy_mtx_init(&g_lang_mtx) != 0)
        return -1;
    return 0;
}

void lang_svc_cleanup(void)
{
    airy_mtx_lock(&g_lang_mtx);
    if (g_lang_gw) {
        airy_lang_gateway_destroy(g_lang_gw);
        g_lang_gw = NULL;
    }
    airy_mtx_unlock(&g_lang_mtx);
    airy_mtx_destroy(&g_lang_mtx);
}

/* 懒创建：启动时不要求 llm_d 已就绪；lang_gateway 内部对校准失败
 * 降级为启发式决策（不影响主流程）。首次调用时创建一次。 */
static airy_lang_gateway_t *lang_gw_get(void)
{
    if (g_lang_gw)
        return g_lang_gw;
    airy_lang_gateway_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.auto_calibrate_on_create = 0; /* 按需校准：llm_d 就绪由调用时序保证 */
    airy_lang_gateway_t *gw = NULL;
    if (airy_lang_gateway_create(&cfg, &gw) == AIRY_EOK && gw) {
        g_lang_gw = gw;
        return gw;
    }
    return NULL;
}

/* think.lang_process：输入标准化（Phase 1+2）。
 * params: {text, model_id?, history_tokens?}
 * result: {raw_input, system_prompt, transformed_input,
 *          reasoning_lang, output_lang, decision_reason, decision_chain} */
void lang_svc_process(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing text string", id);
        return;
    }
    const char *model_id = NULL;
    cJSON *m = cJSON_GetObjectItem(params, "model_id");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model_id = m->valuestring;
    uint32_t history_tokens = 0;
    cJSON *ht = cJSON_GetObjectItem(params, "history_tokens");
    if (cJSON_IsNumber(ht) && ht->valuedouble > 0)
        history_tokens = (uint32_t)ht->valuedouble;

    airy_mtx_lock(&g_lang_mtx);
    airy_lang_gateway_t *gw = lang_gw_get();
    if (!gw) {
        airy_mtx_unlock(&g_lang_mtx);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "lang gateway unavailable", id);
        return;
    }
    airy_canonical_request_t *req = NULL;
    airy_err_t err = airy_lang_gateway_process(gw, text->valuestring, model_id, history_tokens,
                                               &req);
    if (err == AIRY_EOK && req)
        airy_lang_gateway_tick(gw); /* 服务端管理校准周期 */
    airy_mtx_unlock(&g_lang_mtx);
    if (err != AIRY_EOK || !req) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "lang process failed", id);
        return;
    }

    cJSON *res = cJSON_CreateObject();
    if (!res) {
        airy_lang_gateway_free_canonical(req);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(res, "raw_input", req->raw_input ? req->raw_input : "");
    cJSON_AddStringToObject(res, "system_prompt", req->system_prompt ? req->system_prompt : "");
    cJSON_AddStringToObject(res, "transformed_input",
                            req->transformed_input ? req->transformed_input : "");
    cJSON_AddNumberToObject(res, "reasoning_lang", (double)req->routing.reasoning_lang);
    cJSON_AddNumberToObject(res, "output_lang", (double)req->routing.output_lang);
    cJSON_AddStringToObject(res, "decision_reason",
                            req->routing.decision_reason ? req->routing.decision_reason : "");
    cJSON_AddStringToObject(res, "decision_chain",
                            req->routing.decision_chain ? req->routing.decision_chain : "[]");
    cJSON_AddStringToObject(res, "telemetry_json",
                            req->telemetry_json ? req->telemetry_json : "{}");
    airy_lang_gateway_free_canonical(req);
    JSONRPC_SEND_SUCCESS(client_fd, res, id);
}

/* think.lang_postprocess：输出后处理（Phase 3）。
 * params: {text, expected_lang?}
 * result: {text} */
void lang_svc_postprocess(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing text string", id);
        return;
    }
    airy_lang_t expected = AIRY_LANG_UNKNOWN;
    cJSON *el = cJSON_GetObjectItem(params, "expected_lang");
    if (cJSON_IsNumber(el))
        expected = (airy_lang_t)(int)el->valuedouble;

    airy_mtx_lock(&g_lang_mtx);
    airy_lang_gateway_t *gw = lang_gw_get();
    if (!gw) {
        airy_mtx_unlock(&g_lang_mtx);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "lang gateway unavailable", id);
        return;
    }
    char *out = NULL;
    airy_err_t err = airy_lang_gateway_post_process(gw, text->valuestring, expected, &out);
    airy_mtx_unlock(&g_lang_mtx);
    if (err != AIRY_EOK || !out) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "lang postprocess failed", id);
        return;
    }
    cJSON *res = cJSON_CreateObject();
    if (!res) {
        AIRY_FREE(out);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(res, "text", out);
    AIRY_FREE(out);
    JSONRPC_SEND_SUCCESS(client_fd, res, id);
}

/* think.lang_stats：网关统计（可观测性）。
 * params: {}
 * result: 网关统计 JSON */
void lang_svc_stats(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    airy_mtx_lock(&g_lang_mtx);
    airy_lang_gateway_t *gw = lang_gw_get();
    char *json = NULL;
    if (gw)
        airy_lang_gateway_stats(gw, &json);
    airy_mtx_unlock(&g_lang_mtx);
    if (!json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "lang stats unavailable", id);
        return;
    }
    cJSON *obj = cJSON_Parse(json);
    AIRY_FREE(json);
    if (!obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid stats JSON", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
}
