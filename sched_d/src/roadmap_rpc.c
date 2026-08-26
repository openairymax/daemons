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

/* ── plan：三级路由查询 ───────────────────────────────────────────── */
static void roadmap_on_plan(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_roadmap) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "roadmap scheduler not initialized", id);
        return;
    }
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
static int roadmap_plan_parse(cJSON *plan_json, airy_task_plan_t **out_plan)
{
    *out_plan = NULL;
    cJSON *nodes = cJSON_GetObjectItem(plan_json, "nodes");
    int node_n = (nodes && cJSON_IsArray(nodes)) ? cJSON_GetArraySize(nodes) : 0;
    if (node_n <= 0)
        return AIRY_ERR_INVALID_PARAM;

    airy_task_plan_t *plan = (airy_task_plan_t *)AIRY_CALLOC(1, sizeof(airy_task_plan_t));
    if (!plan)
        return AIRY_ERR_OUT_OF_MEMORY;

    cJSON *pid = cJSON_GetObjectItem(plan_json, "task_plan_id");
    if (cJSON_IsString(pid) && pid->valuestring && pid->valuestring[0]) {
        plan->task_plan_id = AIRY_STRDUP(pid->valuestring);
        plan->task_plan_id_len = plan->task_plan_id ? strlen(plan->task_plan_id) : 0;
    }

    plan->task_plan_node_count = (size_t)node_n;
    plan->task_plan_nodes = (airy_task_node_t **)AIRY_CALLOC((size_t)node_n,
                                                             sizeof(airy_task_node_t *));
    if (!plan->task_plan_nodes) {
        plan->task_plan_node_count = 0;
        goto fail;
    }
    for (int i = 0; i < node_n; i++) {
        cJSON *nj = cJSON_GetArrayItem(nodes, i);
        if (!nj)
            continue;
        airy_task_node_t *nd = (airy_task_node_t *)AIRY_CALLOC(1, sizeof(airy_task_node_t));
        if (!nd)
            goto fail;
        plan->task_plan_nodes[i] = nd;
        cJSON *f = cJSON_GetObjectItem(nj, "id");
        if (cJSON_IsString(f) && f->valuestring && f->valuestring[0]) {
            nd->task_node_id = AIRY_STRDUP(f->valuestring);
            nd->task_node_id_len = nd->task_node_id ? strlen(nd->task_node_id) : 0;
        }
        f = cJSON_GetObjectItem(nj, "goal");
        if (cJSON_IsString(f) && f->valuestring)
            nd->task_node_goal = AIRY_STRDUP(f->valuestring);
        f = cJSON_GetObjectItem(nj, "handler");
        if (cJSON_IsString(f) && f->valuestring)
            nd->task_node_handler_name = AIRY_STRDUP(f->valuestring);
        f = cJSON_GetObjectItem(nj, "role");
        if (cJSON_IsString(f) && f->valuestring) {
            nd->task_node_agent_role = AIRY_STRDUP(f->valuestring);
            nd->task_node_role_len = nd->task_node_agent_role ? strlen(nd->task_node_agent_role) : 0;
        }
        cJSON *deps = cJSON_GetObjectItem(nj, "depends");
        int dep_n = (deps && cJSON_IsArray(deps)) ? cJSON_GetArraySize(deps) : 0;
        if (dep_n > 0) {
            nd->task_node_depends_on = (char **)AIRY_CALLOC((size_t)dep_n, sizeof(char *));
            if (!nd->task_node_depends_on)
                goto fail;
            for (int d = 0; d < dep_n; d++) {
                cJSON *dj = cJSON_GetArrayItem(deps, d);
                if (!cJSON_IsString(dj) || !dj->valuestring)
                    continue;
                nd->task_node_depends_on[nd->task_node_depends_count] =
                    AIRY_STRDUP(dj->valuestring);
                if (!nd->task_node_depends_on[nd->task_node_depends_count])
                    goto fail;
                nd->task_node_depends_count++;
            }
        }
    }

    cJSON *entry = cJSON_GetObjectItem(plan_json, "entry_points");
    int entry_n = (entry && cJSON_IsArray(entry)) ? cJSON_GetArraySize(entry) : 0;
    if (entry_n > 0) {
        plan->task_plan_entry_points = (char **)AIRY_CALLOC((size_t)entry_n, sizeof(char *));
        if (!plan->task_plan_entry_points)
            goto fail;
        for (int e = 0; e < entry_n; e++) {
            cJSON *ej = cJSON_GetArrayItem(entry, e);
            if (!cJSON_IsString(ej) || !ej->valuestring)
                continue;
            plan->task_plan_entry_points[plan->task_plan_entry_count] = AIRY_STRDUP(ej->valuestring);
            if (!plan->task_plan_entry_points[plan->task_plan_entry_count])
                goto fail;
            plan->task_plan_entry_count++;
        }
    } else {
        plan->task_plan_entry_points = (char **)AIRY_CALLOC((size_t)node_n, sizeof(char *));
        if (!plan->task_plan_entry_points)
            goto fail;
        for (int i = 0; i < node_n; i++) {
            const airy_task_node_t *nd = plan->task_plan_nodes[i];
            if (!nd || !nd->task_node_id || nd->task_node_depends_count > 0)
                continue;
            plan->task_plan_entry_points[plan->task_plan_entry_count] = AIRY_STRDUP(nd->task_node_id);
            if (!plan->task_plan_entry_points[plan->task_plan_entry_count])
                goto fail;
            plan->task_plan_entry_count++;
        }
    }
    *out_plan = plan;
    return AIRY_SUCCESS;

fail:
    airy_task_plan_free(plan);
    return AIRY_ERR_OUT_OF_MEMORY;
}

static void roadmap_on_absorb(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_roadmap) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "roadmap scheduler not initialized", id);
        return;
    }
    if (!params) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return;
    }

    const char *exec_id = NULL;
    cJSON *eid = cJSON_GetObjectItem(params, "exec_id");
    if (cJSON_IsString(eid) && eid->valuestring && eid->valuestring[0])
        exec_id = eid->valuestring;

    /* 模式 A：蓝图注册 —— plan 为对象 JSON */
    cJSON *plan_json = cJSON_GetObjectItem(params, "plan");
    if (cJSON_IsObject(plan_json)) {
        airy_task_plan_t *plan = NULL;
        int perr = roadmap_plan_parse(plan_json, &plan);
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
        cJSON *result = cJSON_CreateObject();
        if (result) {
            cJSON_AddStringToObject(result, "status", "blueprint_registered");
            JSONRPC_SEND_SUCCESS(client_fd, result, id);
        } else {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        }
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
    cJSON *result = cJSON_CreateObject();
    if (result) {
        cJSON_AddStringToObject(result, "status", "result_absorbed");
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }
}

/* ── roadmap_cancel：取消事件注入（L1 回退 + L2 失效） ──────────────── */
static void roadmap_on_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_roadmap) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "roadmap scheduler not initialized", id);
        return;
    }
    if (!params) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return;
    }
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
    cJSON *result = cJSON_CreateObject();
    if (result) {
        cJSON_AddStringToObject(result, "status", "cancelled");
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
    }
}

/* ── roadmap_replan：蓝图修正（受影响节点回退 + L2 失效） ───────────── */
static void roadmap_on_replan(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_roadmap) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "roadmap scheduler not initialized", id);
        return;
    }
    if (!params) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return;
    }
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

/* ── roadmap_stats：实例状态 ───────────────────────────────────────── */
static void roadmap_on_stats(cJSON *params, int id, airy_sock_t client_fd)
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

/* 事件驱动回调包装（与 main.c 既有方法一致：user_data 携带 socket fd）。
 * 非 static：sched_d main.c 在 method_dispatcher_register 中注册。 */
void on_roadmap_plan_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_plan(params, id, *(airy_sock_t *)user_data);
}

void on_roadmap_absorb_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_absorb(params, id, *(airy_sock_t *)user_data);
}

void on_roadmap_cancel_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_cancel(params, id, *(airy_sock_t *)user_data);
}

void on_roadmap_replan_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_replan(params, id, *(airy_sock_t *)user_data);
}

void on_roadmap_stats_method(cJSON *params, int id, void *user_data)
{
    roadmap_on_stats(params, id, *(airy_sock_t *)user_data);
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
    method_dispatcher_register(d, "roadmap_stats", on_roadmap_stats_method, NULL);
    SVC_LOG_INFO("roadmap_rpc: registered roadmap.* methods (plan/absorb/cancel/replan/stats)");
}
