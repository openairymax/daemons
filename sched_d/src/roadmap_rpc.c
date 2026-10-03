/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file roadmap_rpc.c
 * @brief Roadmap scheduler RPC binding for sched_d (roadmap.* methods).
 *
 * 蓝图调度接线（2026-08-25 修复）：见 roadmap_rpc.h 头注释。三级路由
 * L1/L2/L3 由 airy_roadmap_sched_process 完成；执行结果经 absorb 回灌
 * （PASS+SUCCESS 写 L2 双写），cancel/replan 联动 L1 状态机回退与 L2
 * 条目失效。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "platform.h"
#include "roadmap_rpc.h"
#include "roadmap_sched.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <string.h>

#define ROADMAP_PERSIST_SUBDIR "agentrt/roadmap"
#define ROADMAP_L2_FILE "l2_semantic_cache.json"

static airy_roadmap_sched_t *g_roadmap = NULL;

int roadmap_rpc_init(void)
{
    if (g_roadmap)
        return AIRY_SUCCESS;

    airy_rs_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    AIRY_STRNCPY_TERM(cfg.current_step, "entry", sizeof(cfg.current_step));
    cfg.ttl_days = 7;
    cfg.theta_rs = 550; /* permille；与 CLI/roadmap_sched 默认一致 */
    cfg.enable_multi_plan = false;

    /* L2 语义缓存独立持久化（与 CLI 共用同一数据路径，跨进程共享缓存） */
    static char persist_path[512];
    const char *data_dir = airy_data_dir();
    if (data_dir && *data_dir) {
        __builtin_snprintf(persist_path, sizeof(persist_path), "%s/%s/%s", data_dir,
                           ROADMAP_PERSIST_SUBDIR, ROADMAP_L2_FILE);
        cfg.l2_persist_path = persist_path;
    }

    airy_err_t err = airy_roadmap_sched_create(&cfg, &g_roadmap);
    if (err != AIRY_SUCCESS || !g_roadmap) {
        SVC_LOG_ERROR("roadmap_rpc: airy_roadmap_sched_create failed (err=%d)", (int)err);
        g_roadmap = NULL;
        return (int)err;
    }
    SVC_LOG_INFO("roadmap_rpc: blueprint scheduler ready (L2 persist=%s)",
                 cfg.l2_persist_path ? cfg.l2_persist_path : "(memory-only)");
    return AIRY_SUCCESS;
}

void roadmap_rpc_cleanup(void)
{
    if (g_roadmap) {
        airy_roadmap_sched_destroy(g_roadmap);
        g_roadmap = NULL;
        SVC_LOG_INFO("roadmap_rpc: blueprint scheduler destroyed");
    }
}

int roadmap_rpc_ready(void)
{
    return g_roadmap ? 1 : 0;
}

/* method 共用的就绪/参数守卫与状态结果发射（消除同文件三处重复） */
static bool roadmap_ready(int id, airy_sock_t fd)
{
    if (g_roadmap)
        return true;
    JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR,
                       "roadmap scheduler not initialized", id);
    return false;
}

static bool roadmap_guard(cJSON *params, int id, airy_sock_t fd)
{
    if (!roadmap_ready(id, fd))
        return false;
    if (params)
        return true;
    JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
    return false;
}

static void roadmap_reply_ok(airy_sock_t fd, int id, const char *status)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "status", status);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

/* ── plan：三级路由查询 ───────────────────────────────────────────── */
static void roadmap_on_plan(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!roadmap_ready(id, client_fd))
        return;
    cJSON *input = params ? cJSON_GetObjectItem(params, "input") : NULL;
    if (!cJSON_IsString(input) || !input->valuestring || !input->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing input string", id);
        return;
    }

    char *out_json = NULL;
    airy_rs_dispatch_t dispatch = AIRY_RS_DISPATCH_MISS_L3;
    airy_err_t err = airy_roadmap_sched_process(g_roadmap, input->valuestring, &out_json,
                                                &dispatch);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "roadmap plan failed", id);
        AIRY_FREE(out_json);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        AIRY_FREE(out_json);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    const char *tier = "l3";
    if (dispatch == AIRY_RS_DISPATCH_HIT_L1)
        tier = "l1";
    else if (dispatch == AIRY_RS_DISPATCH_HIT_L2)
        tier = "l2";
    cJSON_AddStringToObject(result, "dispatch", tier);
    cJSON_AddStringToObject(result, "result", out_json ? out_json : "");
    AIRY_FREE(out_json);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── absorb：蓝图注册（plan JSON）或执行结果回灌（meta 字段） ──────── */
static void roadmap_on_absorb(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!roadmap_guard(params, id, client_fd))
        return;

    const char *exec_id = NULL;
    cJSON *eid = cJSON_GetObjectItem(params, "exec_id");
    if (cJSON_IsString(eid) && eid->valuestring && eid->valuestring[0])
        exec_id = eid->valuestring;

    /* 模式 A：蓝图注册 —— plan 为对象 JSON（解析单源：airy_plan_parse） */
    cJSON *plan_json = cJSON_GetObjectItem(params, "plan");
    if (cJSON_IsObject(plan_json)) {
        airy_task_plan_t *plan = NULL;
        char *plan_str = cJSON_PrintUnformatted(plan_json);
        airy_err_t perr = plan_str ? airy_plan_parse(plan_str, &plan)
                                   : AIRY_ERR_OUT_OF_MEMORY;
        AIRY_FREE(plan_str);
        if (perr != AIRY_SUCCESS || !plan) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid plan JSON", id);
            return;
        }
        airy_err_t err = airy_roadmap_sched_absorb(g_roadmap, plan, exec_id, NULL);
        airy_task_plan_free(plan);
        if (err != AIRY_SUCCESS) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "roadmap absorb plan failed",
                               id);
            return;
        }
        roadmap_reply_ok(client_fd, id, "blueprint_registered");
        return;
    }

    /* 模式 B：执行结果回灌（exec_id + node_id + output_json + result/verify） */
    cJSON *node_id = cJSON_GetObjectItem(params, "node_id");
    if (!cJSON_IsString(node_id) || !node_id->valuestring || !node_id->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing node_id (or plan object)",
                           id);
        return;
    }
    airy_rs_absorb_meta_t meta;
    __builtin_memset(&meta, 0, sizeof(meta));
    meta.node_id = node_id->valuestring;
    cJSON *out_json = cJSON_GetObjectItem(params, "output_json");
    if (cJSON_IsString(out_json))
        meta.output_json = out_json->valuestring;
    cJSON *result_v = cJSON_GetObjectItem(params, "result");
    if (cJSON_IsNumber(result_v))
        meta.result = (airy_rs_result_t)result_v->valueint;
    cJSON *verify_v = cJSON_GetObjectItem(params, "verify");
    if (cJSON_IsNumber(verify_v))
        meta.verify = (airy_rs_verify_t)verify_v->valueint;
    cJSON *transient_v = cJSON_GetObjectItem(params, "transient");
    if (cJSON_IsTrue(transient_v))
        meta.transient = true;
    cJSON *canceled_v = cJSON_GetObjectItem(params, "canceled");
    if (cJSON_IsTrue(canceled_v))
        meta.canceled = true;
    cJSON *user_intent = cJSON_GetObjectItem(params, "is_user_intent");
    if (cJSON_IsTrue(user_intent))
        meta.is_user_intent = true;

    airy_err_t err = airy_roadmap_sched_absorb(g_roadmap, NULL, exec_id, &meta);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "roadmap absorb result failed", id);
        return;
    }
    roadmap_reply_ok(client_fd, id, "result_absorbed");
}

/* ── roadmap_cancel：取消事件注入（L1 回退 + L2 失效） ──────────────── */
static void roadmap_on_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!roadmap_guard(params, id, client_fd))
        return;
    const char *exec_id = NULL;
    cJSON *eid = cJSON_GetObjectItem(params, "exec_id");
    if (cJSON_IsString(eid) && eid->valuestring && eid->valuestring[0])
        exec_id = eid->valuestring;
    cJSON *node_id = cJSON_GetObjectItem(params, "node_id");
    if (!cJSON_IsString(node_id) || !node_id->valuestring || !node_id->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing node_id", id);
        return;
    }
    airy_err_t err = airy_roadmap_sched_on_cancel(g_roadmap, exec_id, node_id->valuestring);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "roadmap cancel failed", id);
        return;
    }
    roadmap_reply_ok(client_fd, id, "cancelled");
}

/* ── roadmap_replan：蓝图修正（受影响节点回退 + L2 失效） ───────────── */
static void roadmap_on_replan(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!roadmap_guard(params, id, client_fd))
        return;
    cJSON *affected = cJSON_GetObjectItem(params, "affected_nodes");
    int affected_n = (affected && cJSON_IsArray(affected)) ? cJSON_GetArraySize(affected) : 0;
    if (affected_n <= 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "affected_nodes must be a non-empty array", id);
        return;
    }
    char **nodes = (char **)AIRY_CALLOC((size_t)affected_n, sizeof(char *));
    if (!nodes) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    int n = 0;
    for (int i = 0; i < affected_n; i++) {
        cJSON *aj = cJSON_GetArrayItem(affected, i);
        if (!cJSON_IsString(aj) || !aj->valuestring)
            continue;
        nodes[n++] = AIRY_STRDUP(aj->valuestring);
    }
    if (n <= 0) {
        for (int i = 0; i < n; i++)
            AIRY_FREE(nodes[i]);
        AIRY_FREE(nodes);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "affected_nodes empty", id);
        return;
    }
    const char *reason = NULL;
    cJSON *rz = cJSON_GetObjectItem(params, "replan_reason");
    if (cJSON_IsString(rz) && rz->valuestring)
        reason = rz->valuestring;

    airy_rs_replan_ctx_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    ctx.affected_nodes = (const char *const *)nodes;
    ctx.affected_count = (size_t)n;
    ctx.replan_reason = reason;

    char **rerun = NULL;
    size_t rerun_count = 0;
    airy_err_t err = airy_roadmap_sched_replan(g_roadmap, &ctx, &rerun, &rerun_count);
    for (int i = 0; i < n; i++)
        AIRY_FREE(nodes[i]);
    AIRY_FREE(nodes);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "roadmap replan failed", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        for (size_t i = 0; i < rerun_count; i++)
            AIRY_FREE(rerun[i]);
        AIRY_FREE(rerun);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "status", "replanned");
    cJSON *rn = cJSON_CreateArray();
    for (size_t i = 0; i < rerun_count; i++) {
        if (rerun[i])
            cJSON_AddItemToArray(rn, cJSON_CreateString(rerun[i]));
        AIRY_FREE(rerun[i]);
    }
    AIRY_FREE(rerun);
    cJSON_AddItemToObject(result, "rerun_nodes", rn);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── roadmap_status：实例状态（V8.3 名实一致：仅就绪态与
 * 服务标识，不含计数统计，故不名 stats）──────────────────────────── */
static void roadmap_on_status(cJSON *params, int id, airy_sock_t client_fd)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddBoolToObject(result, "ready", g_roadmap ? 1 : 0);
    cJSON_AddStringToObject(result, "service", "sched_d.roadmap");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* 事件驱动回调包装（method_fn 签名；user_data 携带客户端 socket fd）。
 * 注册内聚于 roadmap_rpc_register，勿在外部引用。 */
static void on_roadmap_plan_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_plan(params, id, *(airy_sock_t *)user_data);
}

static void on_roadmap_absorb_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_absorb(params, id, *(airy_sock_t *)user_data);
}

static void on_roadmap_cancel_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_cancel(params, id, *(airy_sock_t *)user_data);
}

static void on_roadmap_replan_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_replan(params, id, *(airy_sock_t *)user_data);
}

static void on_roadmap_status_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_status(params, id, *(airy_sock_t *)user_data);
}

void roadmap_rpc_register(void *disp)
{
    method_dispatcher_t *d = (method_dispatcher_t *)disp;
    if (!d)
        return;
    method_dispatcher_register(d, "plan", on_roadmap_plan_method, NULL);
    method_dispatcher_register(d, "absorb", on_roadmap_absorb_method, NULL);
    method_dispatcher_register(d, "roadmap_cancel", on_roadmap_cancel_method, NULL);
    method_dispatcher_register(d, "roadmap_replan", on_roadmap_replan_method, NULL);
    method_dispatcher_register(d, "roadmap_status", on_roadmap_status_method, NULL);
    SVC_LOG_INFO("roadmap_rpc: registered roadmap methods (plan/absorb/cancel/replan/status)");
}
