// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file adapter.c
 * @brief 通用 completion driver（B16-S3.3）：适配层四槽入口的唯一编排实现。
 *
 * 密钥刷新 → build_request（流式置 stream=1）→ provider_http_exec 出网
 * → 错误归一（provider_http_err_map / err_diag）→ parse_response 或流末
 * take+flush 装配 → 统一日志。适配器（adapters/）以两行 shim 传入自身
 * ops 调用本件，core 不出现任何厂商名分支——V16.4"core 零厂商特判"
 * 的结构保证。失败路径 http_resp 所有权：非限流失败时 provider_http_post
 * 已内部释放（指针保持 NULL）；限流失败 4xx 路径 resp 落地归调用方释放
 * ——统一防御式 free 覆盖两种模式。
 */

#include <string.h>

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "adapter.h"
#include "provider_rec.h"
#include "prov_diag.h"
#include "provider_http.h"
#include "secrets.h"
#include "toolstream.h"
#include "svc_logger.h"

/* 模型名解析：请求显式值优先，其次适配器默认（可为 NULL），最终空串，
 * 保证日志 %s 永不见 NULL。 */
static const char *req_model(const provider_adapter_t *ops, const llm_request_config_t *manager)
{
    if (manager->model && manager->model[0])
        return manager->model;
    return ops->default_model ? ops->default_model : "";
}

int prov_max_conc(const provider_t *prov, const char *model)
{
    if (!prov || !model || !prov->models || !prov->model_max_concurrent)
        return 0;
    for (size_t i = 0; prov->models[i]; ++i) {
        if (strcmp(prov->models[i], model) == 0)
            return prov->model_max_concurrent[i];
    }
    return 0;
}

/* B1-d：失败 resp 释放前把上游状态码与响应体片段存入线程诊断槽，
 * 供 RPC 错误面透传（resp 可为 NULL——传输层失败时指针已归一为空）。 */
static void diag_from_resp(long http_code, const provider_http_resp_t *resp)
{
    const char *body = (resp && resp->data) ? resp->data : NULL;
    prov_diag_set(http_code, body, body ? strlen(body) : 0);
}

/* C11 流末完整性判定：机制面只报告事实（缺失=length 之外的非完备
 * finish_reason），处置归调用方策略侧。stop/tool_calls 为合法完备集；
 * [DONE] 缺失由 sse 层判，本件不重复。resp 必须非 NULL。 */
static int stream_integrity(const llm_response_t *resp)
{
    const char *fr = resp->finish_reason;
    if (!fr)
        return AIRY_ERR_LLM_NO_FINISH;
    if (llm_finish_is_truncated(fr))
        return AIRY_ERR_LLM_TRUNCATED;
    if (strcmp(fr, LLM_FINISH_CONTENT_FILTER) == 0)
        return AIRY_ERR_LLM_CONTENT_FILTERED;
    return AIRY_OK;
}

/* D2-9：端点容量随响应回传。仅在探测成功（props_state=1）时盖章，未探测
 * 端点保持 0——客户端按存在性门控，不猜默认值。 */
static void resp_stamp_capacity(llm_response_t *resp, const provider_base_ctx_t *base)
{
    if (!resp || !base || base->props_state != 1)
        return;
    if (base->n_ctx_slot > 0)
        resp->context_window = (uint32_t)base->n_ctx_slot;
    if (base->total_slots > 0)
        resp->total_slots = (uint32_t)base->total_slots;
}

/* D2-9：幂等判据（方案 §5.2b.10）——确定性采样下重复投递语义等价，
 * 传输侧才允许在"请求可能已送达"的故障形态上重试。 */
static int req_idem(const llm_request_config_t *cfg)
{
    return cfg->temperature == 0.0f && cfg->seed != 0;
}

/* 成功往返的墙钟时延入 p99 样本。失败/超时不入样：否则预算会被失败样本
 * 自我抬升（超时次数另有 transport 计数可观测）。 */
static void lat_note(const provider_base_ctx_t *base, uint64_t start_ms)
{
    if (!base || !base->name[0])
        return;
    prov_stat_lat(base->name, (uint32_t)(airy_time_ms() - start_ms));
}

/* 入口前置条件：四件公共依赖 + 构体槽；sink 为各形态独有槽位（同步为
 * 输出槽，流式为回调）。 */
static int arg_ok(const provider_ctx_t *ctx, const provider_adapter_t *ops,
                  const llm_request_config_t *manager, const void *sink)
{
    return ctx && ops && manager && sink && ops->build_request;
}

/* 请求体装配失败统一收口：phase 仅作日志区分（COMPLETE / STREAM），
 * stack 指向失败点所在函数名。 */
static char *body_build(const provider_adapter_t *ops, const llm_request_config_t *cfg,
                        const char *phase, const char *stack)
{
    char *body = ops->build_request(cfg);

    if (!body)
        SVC_LOG_ERROR("C-L02: %s: %s-FAIL model=%s reason=build_request_oom STACK: %s",
                      ops->tag, phase, req_model(ops, cfg), stack);
    return body;
}

/* 失败统一收口：日志、诊断槽存证、响应体释放。传输失败与非 200 状态码
 * 两条路径共用——两副本曾各自维护，漏改一侧即丢诊断。 */
static int req_fail(const provider_adapter_t *ops, const char *model, long http_code,
                    provider_http_resp_t *resp, int ret)
{
    SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s http_code=%ld ret=%d DIAGNOSIS=%s "
                  "body=%.600s",
                  ops->tag, model, http_code, ret, provider_http_err_diag(http_code),
                  resp && resp->data ? resp->data : "");
    diag_from_resp(http_code, resp);
    if (resp)
        provider_http_resp_free(resp);
    return ret;
}

/* 成功路径响应装配：解析 → 端点容量盖章 → 结果日志。resp 所有权在本件
 * 内终结，解析结果转出至 out_response。 */
static int complete_parse(const provider_adapter_t *ops, const provider_base_ctx_t *base,
                          const char *model, provider_http_resp_t *resp, long http_code,
                          llm_response_t **out_response)
{
    int ret = ops->parse_response(resp->data, out_response);

    provider_http_resp_free(resp);

    if (ret != AIRY_OK || !*out_response) {
        SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s http_code=%ld "
                      "DIAGNOSIS=parse_response_failed ret=%d",
                      ops->tag, model, http_code, ret);
        return ret;
    }

    resp_stamp_capacity(*out_response, base);
    SVC_LOG_INFO("C-L02: %s: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                 "http_code=%ld",
                 ops->tag, (*out_response)->model ? (*out_response)->model : model,
                 (*out_response)->prompt_tokens, (*out_response)->completion_tokens,
                 (*out_response)->total_tokens, http_code);
    return ret;
}

/* 流末收口：取累加响应 → 残留工具调用冲刷 → 累加器释放 → 完备性判定
 * → 端点容量盖章 → 结果转出。完整性缺失即整条流作废，不留半成品。 */
static int stream_finish(const provider_adapter_t *ops, const provider_base_ctx_t *base,
                         const char *model, long http_code, provider_stream_acc_t *acc,
                         llm_stream_callback_t callback, void *callback_data,
                         llm_response_t **out_response)
{
    llm_response_t *resp = provider_stream_take(acc);

    provider_tool_flush(&acc->tools, resp, callback, callback_data);
    provider_stream_acc_free(acc);

    int integrity = resp ? stream_integrity(resp) : AIRY_OK;

    if (integrity != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: %s: STREAM-FAIL model=%s http_code=%ld reason=integrity ret=%d",
                      ops->tag, model, http_code, integrity);
        llm_response_free(resp);
        if (out_response)
            *out_response = NULL;
        return integrity;
    }

    if (resp) {
        resp_stamp_capacity(resp, base);
        SVC_LOG_INFO("C-L02: %s: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "http_code=%ld",
                     ops->tag, resp->model ? resp->model : model, resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens, http_code);
    } else {
        SVC_LOG_WARN("C-L02: %s: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=null_response_built",
                     ops->tag, model, http_code);
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        llm_response_free(resp);

    return AIRY_OK;
}

int provider_driver_complete(provider_ctx_t *ctx, const provider_adapter_t *ops,
                             const llm_request_config_t *manager, llm_response_t **out_response)
{
    if (!arg_ok(ctx, ops, manager, out_response) || !ops->parse_response)
        return AIRY_ERR_INVALID_PARAM;

    provider_base_ctx_t *base = provider_base_ctx(ctx);
    provider_refresh_api_key(base);

    /* C11：能力位由 base 盖印进请求快照（driver 是唯一盖印点，
     * 调用方与 envelope 之间的机制约定）。 */
    llm_request_config_t eff = *manager;
    eff.caps = base->caps;

    char *req_body = body_build(ops, &eff, "COMPLETE", "provider_driver_complete");
    if (!req_body)
        return AIRY_ERR_OUT_OF_MEMORY;

    const char *model = req_model(ops, manager);
    SVC_LOG_INFO("C-L02: %s: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f stream=%d",
                 ops->tag, model, manager->message_count, manager->max_tokens,
                 manager->temperature, manager->stream ? 1 : 0);

    provider_request_t req = {
        .base = base,
        .rl = ops->rate_limiter ? ops->rate_limiter(ctx) : NULL,
        .path = ops->chat_path,
        .headers = ops->headers,
        .body = req_body,
        .idempotent = req_idem(&eff),
    };

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;
    uint64_t t0 = airy_time_ms();
    int ret = provider_http_exec(&req, &http_resp, &http_code);
    if (ret == AIRY_OK)
        lat_note(base, t0);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK)
        return req_fail(ops, model, http_code, http_resp, ret);

    /* 非限流模式：exec 返回 OK 仅代表传输成功，成败看状态码（4xx/5xx 响应体
     * 对调用方有诊断价值，exec 不吞）；限流模式此分支不可达（非 200 已在
     * core 归一为错误码返回）。 */
    if (http_code != 200)
        return req_fail(ops, model, http_code, http_resp,
                        provider_http_err_map(http_code, AIRY_ERR_IO));

    return complete_parse(ops, base, model, http_resp, http_code, out_response);
}

int provider_driver_complete_stream(provider_ctx_t *ctx, const provider_adapter_t *ops,
                                    const llm_request_config_t *manager,
                                    llm_stream_callback_t callback, void *callback_data,
                                    llm_response_t **out_response)
{
    if (!arg_ok(ctx, ops, manager, callback))
        return AIRY_ERR_INVALID_PARAM;

    provider_base_ctx_t *base = provider_base_ctx(ctx);
    provider_refresh_api_key(base);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;
    stream_cfg.caps = base->caps;

    char *req_body = body_build(ops, &stream_cfg, "STREAM", "provider_driver_complete_stream");
    if (!req_body)
        return AIRY_ERR_OUT_OF_MEMORY;

    const char *model = req_model(ops, manager);
    SVC_LOG_INFO("C-L02: %s: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", ops->tag,
                 model, manager->message_count, manager->max_tokens, manager->temperature);

    provider_stream_acc_t acc;
    provider_stream_acc_init(&acc, callback, callback_data);

    /* 分帧模式由 feed_event 槽唯一决定：非 NULL 为具名事件流，
     * NULL 为行协议流（on_chunk 机制件）。 */
    provider_request_t req = {
        .base = base,
        .path = ops->chat_path,
        .headers = ops->headers,
        .body = req_body,
        .on_chunk = ops->feed_event ? NULL : prov_stream_chunk,
        .on_event = ops->feed_event,
        .user_data = &acc,
        .idempotent = req_idem(&stream_cfg),
    };

    long http_code = 0;
    uint64_t t0 = airy_time_ms();
    int ret = provider_http_exec(&req, NULL, &http_code);
    if (ret == AIRY_OK)
        lat_note(base, t0);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: %s: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=%s", ops->tag,
                      model, http_code, provider_http_err_diag(http_code));
        provider_stream_acc_free(&acc);
        return provider_http_err_map(http_code, ret);
    }

    return stream_finish(ops, base, model, http_code, &acc, callback, callback_data, out_response);
}
