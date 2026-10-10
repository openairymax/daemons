/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief mem_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 记忆服务单例（service/cache/ledger）+ 生命周期五钩子 + RPC 蹦床。
 * 可配置户：端点基线取自生成头常量（MEM_D_SOCKET_UNIX/WIN、
 * MEM_D_TCP_PORT），端点族委托 daemon_cfg_file 机制件
 * （daemon_ep_load/free/apply）；策略键经 cfg_keys 从 config 根对象
 * 自持提取——daemon 段 max_records、"compress" 段 B5 门禁、"token"
 * 段计数模型；环境变量覆盖先于 JSON（daemon_ep_load 内文件覆盖式
 * 解析）。声明式策略缺省 fail-closed，改动不触发重编译。业务逻辑
 * 在 handlers 四域。
 *
 * 蹦床仅做 user_data → airy_sock_t 的适配，无业务逻辑。
 */

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

/* g_config 只留业务策略键；端点配置由 daemon_cfg_file 机制槽唯一持有 */

/* ── RPC 蹦床（m_<method> ↔ .manifest rpc.methods） ─────────────────────── */

DAEMON_RPC_SHELL(write, handle_write)
DAEMON_RPC_SHELL(search, handle_search)
DAEMON_RPC_SHELL(get, handle_get)
DAEMON_RPC_SHELL(delete, handle_delete)
DAEMON_RPC_SHELL0(count, handle_count)
DAEMON_RPC_SHELL(recent, handle_recent)
DAEMON_RPC_SHELL(evolve, handle_evolve)
DAEMON_RPC_SHELL0(health_check, handle_health_check)
DAEMON_RPC_SHELL0(get_stats, handle_get_stats)
DAEMON_RPC_SHELL(kb_ingest, handle_kb_ingest)
DAEMON_RPC_SHELL(kb_search, handle_kb_search)
DAEMON_RPC_SHELL(kb_delete, handle_kb_delete)
DAEMON_RPC_SHELL(kb_list, handle_kb_list)
DAEMON_RPC_SHELL(cache_put, handle_cache_put)
DAEMON_RPC_SHELL(cache_get, handle_cache_get)
DAEMON_RPC_SHELL(cache_del, handle_cache_del)
DAEMON_RPC_SHELL0(cache_stats, handle_cache_stats)
DAEMON_RPC_SHELL(ledger_append, handle_ledger_append)
DAEMON_RPC_SHELL(ledger_window, handle_ledger_window)
DAEMON_RPC_SHELL(ledger_budget, handle_ledger_budget)
DAEMON_RPC_SHELL(ledger_mark, handle_ledger_mark)
DAEMON_RPC_SHELL(ledger_history, handle_ledger_history)
DAEMON_RPC_SHELL0(ledger_stats, handle_ledger_stats)
DAEMON_RPC_SHELL(compress, handle_compress)

/* ── 配置装载（声明式策略注入） ─────────────────────────────────────────── */

/* 策略键派发（daemon_ep_load 回调）：端点三元组由机制件收整；
 * max_records 与 B5 门禁/模型从根对象自持提取，缺省字段保持
 * fail-closed 初值。 */
static void cfg_keys(const cJSON *root, void *ud)
{
    (void)ud;
    cJSON *item = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "daemon"),
                                      "max_records");
    if (cJSON_IsNumber(item))
        g_config.max_records = (size_t)item->valuedouble;
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

    daemon_ep_load(daemon_ep_slot(), config_path, MEM_D_SOCKET_UNIX,
                   MEM_D_SOCKET_WIN, MEM_D_TCP_PORT, cfg_keys, NULL);
}

/* ── 生命周期五钩子（实现 generated main.c 契约） ───────────────────────── */

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);

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
    daemon_ep_free(daemon_ep_slot());
}

/* 策略层附加装配挂点：mem_d 的 mem.* 方法面已由生成态静态注册表
 * （SVC_METHODS）全量覆盖，无附加动态注册族，提供空实现以满足契约。 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
