/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief mem_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 记忆服务单例（service/cache/ledger）+ 生命周期五钩子 + RPC 蹦床。
 * 可配置户：端点基线取自生成头常量（MEM_D_SOCKET_UNIX/WIN、
 * MEM_D_TCP_PORT），-c JSON 的 daemon 段可覆盖 socket_path / tcp_port /
 * max_clients / max_records；cmdline use_tcp 只升不降。压缩门禁与台账
 * 计数模型为声明式策略（config "compress"/"token" 段 + 环境变量覆盖，
 * 缺省 fail-closed），改动不触发重编译。业务逻辑在 handlers 四域。
 *
 * 蹦床仅做 user_data → airy_sock_t 的适配，无业务逻辑。
 */

#include "daemon_main.h"
#include "platform.h"
#include "svc_logger.h"
#include "svc_mem_d.h"

#include "mem_service.h"
#include "cache.h"
#include "ledger.h"
#include "mem_daemon_ctx.h"
#include "mem_handlers.h"
#include "kb_handlers.h"
#include "cache_handlers.h"
#include "ledger_handlers.h"

#include "airy_memory.h"
#include "daemon_cfg_file.h"
#include "error.h"

#include <stdlib.h>

/* ── 单例（handler 四域经 mem_daemon_ctx.h 共享） ────────────────────────── */

mem_service_t *g_service = NULL;
mem_cache_t *g_cache = NULL;
mem_ledger_t *g_ledger = NULL;

mem_daemon_config_t g_config = {0};

/* ── RPC 蹦床（m_<method> ↔ .manifest rpc.methods） ─────────────────────── */

void m_write(cJSON *params, int id, void *user_data)
{ handle_write(params, id, *(airy_sock_t *)user_data); }

void m_search(cJSON *params, int id, void *user_data)
{ handle_search(params, id, *(airy_sock_t *)user_data); }

void m_get(cJSON *params, int id, void *user_data)
{ handle_get(params, id, *(airy_sock_t *)user_data); }

void m_delete(cJSON *params, int id, void *user_data)
{ handle_delete(params, id, *(airy_sock_t *)user_data); }

void m_count(cJSON *params, int id, void *user_data)
{ (void)params; handle_count(id, *(airy_sock_t *)user_data); }

void m_recent(cJSON *params, int id, void *user_data)
{ handle_recent(params, id, *(airy_sock_t *)user_data); }

void m_evolve(cJSON *params, int id, void *user_data)
{ handle_evolve(params, id, *(airy_sock_t *)user_data); }

void m_health_check(cJSON *params, int id, void *user_data)
{ (void)params; handle_health_check(id, *(airy_sock_t *)user_data); }

void m_get_stats(cJSON *params, int id, void *user_data)
{ (void)params; handle_get_stats(id, *(airy_sock_t *)user_data); }

void m_kb_ingest(cJSON *params, int id, void *user_data)
{ handle_kb_ingest(params, id, *(airy_sock_t *)user_data); }

void m_kb_search(cJSON *params, int id, void *user_data)
{ handle_kb_search(params, id, *(airy_sock_t *)user_data); }

void m_kb_delete(cJSON *params, int id, void *user_data)
{ handle_kb_delete(params, id, *(airy_sock_t *)user_data); }

void m_kb_list(cJSON *params, int id, void *user_data)
{ handle_kb_list(params, id, *(airy_sock_t *)user_data); }

void m_cache_put(cJSON *params, int id, void *user_data)
{ handle_cache_put(params, id, *(airy_sock_t *)user_data); }

void m_cache_get(cJSON *params, int id, void *user_data)
{ handle_cache_get(params, id, *(airy_sock_t *)user_data); }

void m_cache_del(cJSON *params, int id, void *user_data)
{ handle_cache_del(params, id, *(airy_sock_t *)user_data); }

void m_cache_stats(cJSON *params, int id, void *user_data)
{ (void)params; handle_cache_stats(id, *(airy_sock_t *)user_data); }

void m_ledger_append(cJSON *params, int id, void *user_data)
{ handle_ledger_append(params, id, *(airy_sock_t *)user_data); }

void m_ledger_window(cJSON *params, int id, void *user_data)
{ handle_ledger_window(params, id, *(airy_sock_t *)user_data); }

void m_ledger_budget(cJSON *params, int id, void *user_data)
{ handle_ledger_budget(params, id, *(airy_sock_t *)user_data); }

void m_ledger_mark(cJSON *params, int id, void *user_data)
{ handle_ledger_mark(params, id, *(airy_sock_t *)user_data); }

void m_ledger_history(cJSON *params, int id, void *user_data)
{ handle_ledger_history(params, id, *(airy_sock_t *)user_data); }

void m_ledger_stats(cJSON *params, int id, void *user_data)
{ (void)params; handle_ledger_stats(id, *(airy_sock_t *)user_data); }

void m_compress(cJSON *params, int id, void *user_data)
{ handle_compress(params, id, *(airy_sock_t *)user_data); }

/* ── 配置装载（声明式策略注入） ─────────────────────────────────────────── */

static void cfg_on_load(cJSON *root, void *ud)
{
    daemon_ep_cfg_t *ep = (daemon_ep_cfg_t *)ud;
    cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
    if (daemon_cfg) {
        daemon_ep_parse(daemon_cfg, ep);
        cJSON *item = cJSON_GetObjectItem(daemon_cfg, "max_clients");
        if (cJSON_IsNumber(item))
            g_config.max_clients = item->valueint;
        item = cJSON_GetObjectItem(daemon_cfg, "max_records");
        if (cJSON_IsNumber(item))
            g_config.max_records = (size_t)item->valuedouble;
    }
    /* B5：压缩门禁策略声明（缺省字段保持 fail-closed 初值） */
    cJSON *ccfg = cJSON_GetObjectItem(root, "compress");
    if (ccfg) {
        cJSON *l1 = cJSON_GetObjectItem(ccfg, "l1_enabled");
        if (cJSON_IsBool(l1))
            g_config.compress_l1_enabled = cJSON_IsTrue(l1) ? 1 : 0;
        cJSON *l2 = cJSON_GetObjectItem(ccfg, "l2_enabled");
        if (cJSON_IsBool(l2))
            g_config.compress_l2_enabled = cJSON_IsTrue(l2) ? 1 : 0;
        cJSON *gate = cJSON_GetObjectItem(ccfg, "gate");
        if (gate) {
            cJSON *gs = cJSON_GetObjectItem(gate, "grayscale");
            if (cJSON_IsBool(gs))
                g_config.compress_gate_grayscale = cJSON_IsTrue(gs) ? 1 : 0;
            cJSON *acr = cJSON_GetObjectItem(gate, "acr");
            if (cJSON_IsNumber(acr))
                g_config.compress_gate_acr = acr->valuedouble;
            cJSON *ttft = cJSON_GetObjectItem(gate, "ttft_ms");
            if (cJSON_IsNumber(ttft))
                g_config.compress_gate_ttft_ms = ttft->valuedouble;
        }
    }
    /* B5：台账计数模型声明（缺省 → 默认模型，不得硬编码） */
    cJSON *tcfg = cJSON_GetObjectItem(root, "token");
    if (tcfg) {
        cJSON *model = cJSON_GetObjectItem(tcfg, "model");
        if (cJSON_IsString(model) && model->valuestring[0])
            AIRY_STRNCPY_TERM(g_config.token_model, model->valuestring,
                              sizeof(g_config.token_model));
    }
}

static void load_daemon_config(const char *config_path)
{
    daemon_ep_cfg_t ep;
    daemon_ep_def(MEM_D_SOCKET_UNIX, MEM_D_SOCKET_WIN, MEM_D_TCP_PORT, &ep);
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_records = MEM_DEFAULT_MAX_RECORDS;
    /* B5：L2 压缩门禁默认 fail-closed（L2 关、灰度关、acr/ttft 不可用） */
    g_config.compress_l1_enabled = 1;
    g_config.compress_l2_enabled = 0;
    g_config.compress_gate_grayscale = 0;
    g_config.compress_gate_acr = -1.0;
    g_config.compress_gate_ttft_ms = -1.0;
    /* B5：台账计数模型默认空（由 token_standard 默认模型兜底） */
    g_config.token_model[0] = '\0';

    const char *env = getenv("AIRY_MEM_MAX_RECORDS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_records = (size_t)v;
    }

    /* B5：压缩门禁策略可用环境变量翻转（策略值改动不触发重编译） */
    const char *env_l2 = getenv("AIRY_MEM_COMPRESS_L2");
    if (env_l2)
        g_config.compress_l2_enabled = strtoul(env_l2, NULL, 10) != 0 ? 1 : 0;
    const char *env_gs = getenv("AIRY_MEM_COMPRESS_GATE_GRAYSCALE");
    if (env_gs)
        g_config.compress_gate_grayscale = strtoul(env_gs, NULL, 10) != 0 ? 1 : 0;
    const char *env_acr = getenv("AIRY_MEM_COMPRESS_GATE_ACR");
    if (env_acr) {
        char *end = NULL;
        double v = strtod(env_acr, &end);
        if (end != env_acr)
            g_config.compress_gate_acr = v;
    }
    const char *env_ttft = getenv("AIRY_MEM_COMPRESS_GATE_TTFT_MS");
    if (env_ttft) {
        char *end = NULL;
        double v = strtod(env_ttft, &end);
        if (end != env_ttft)
            g_config.compress_gate_ttft_ms = v;
    }

    /* B5：台账计数模型（按实际模型取，缺省回退默认模型） */
    const char *env_model = getenv("AIRY_MEM_TOKEN_MODEL");
    if (env_model && env_model[0])
        AIRY_STRNCPY_TERM(g_config.token_model, env_model, sizeof(g_config.token_model));

    daemon_cfg_read(config_path, cfg_on_load, &ep);
    g_config.socket_path = ep.socket_path;
    g_config.tcp_port = (uint16_t)ep.tcp_port;
    g_config.use_tcp = ep.use_tcp;
}

static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_MEMSET(&g_config, 0, sizeof(g_config));
}

/* ── 生命周期五钩子（实现 generated main.c 契约） ───────────────────────── */

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp ? 1 : (g_config.use_tcp ? 1 : 0);
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = g_config.tcp_port;
    ep->sock_unix = g_config.socket_path;
    ep->sock_win = g_config.socket_path;

    if (ep->use_tcp)
        SVC_LOG_INFO("Listening on TCP %s:%u", ep->tcp_host, (unsigned)ep->tcp_port);
    else
        SVC_LOG_INFO("Listening on %s", ep->sock_unix ? ep->sock_unix : "(unset)");
}

int svc_prepare(const char *config_path)
{
    load_daemon_config(config_path);

    SVC_LOG_INFO("Memory service starting, manager=%s", config_path ? config_path : "default");
    SVC_LOG_INFO("compress policy: l1=%d l2=%d gate{grayscale=%d acr=%.3f ttft=%.1fms}",
                 g_config.compress_l1_enabled, g_config.compress_l2_enabled,
                 g_config.compress_gate_grayscale, g_config.compress_gate_acr,
                 g_config.compress_gate_ttft_ms);

    g_service = mem_service_create(g_config.max_records);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create memory service");
        return -1;
    }

    /* 0.1.5：语义缓存 + 上下文台账（渐进式降级：创建失败仅告警，不阻断服务） */
    g_cache = mem_cache_create(4096, 64UL * 1024 * 1024, 3600000ULL, 0.85);
    if (!g_cache)
        SVC_LOG_WARN("Semantic cache init failed, caching disabled (degraded mode)");
    g_ledger = mem_ledger_create(0, 0);
    if (!g_ledger) {
        SVC_LOG_WARN("Context ledger init failed, ledger disabled (degraded mode)");
    } else {
        /* B5：按声明注入计数模型（空 → 默认模型），失败保留默认模型 */
        int model_ret = mem_ledger_set_token_model(g_ledger, g_config.token_model);
        if (model_ret != AIRY_SUCCESS)
            SVC_LOG_WARN("Token model '%s' rejected (%d), fallback kept: %s",
                         g_config.token_model[0] ? g_config.token_model : "(default)", model_ret,
                         mem_ledger_token_model(g_ledger));
        SVC_LOG_INFO("ledger token model: %s", mem_ledger_token_model(g_ledger));
    }
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        mem_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_cache) {
        mem_cache_destroy(g_cache);
        g_cache = NULL;
    }
    if (g_ledger) {
        mem_ledger_destroy(g_ledger);
        g_ledger = NULL;
    }
    free_daemon_config();
}

/* 策略层附加装配挂点：mem_d 的 mem.* 方法面已由生成态静态注册表
 * （SVC_METHODS）全量覆盖，无附加动态注册族，提供空实现以满足契约。 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
