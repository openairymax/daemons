/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_cap_registry.c
 * @brief 统一能力网关 — 能力注册表实现（0.1.6 P1-4）。
 *
 * 能力注册表是网关对外暴露能力的唯一权威枚举（SSoT）。表项来源于
 * 原 gateway_biz_forward.c 的 15 个命名空间白名单数组 + 各特殊处理器，
 * 收敛为单一声明；任何外部可调用能力必须在此登记（fail-closed）。
 *
 * 表内顺序：按命名空间分组，FWD 转发项在前、特殊项随命名空间就近放置。
 * 查询为线性查找（能力数 < 200，注册表常驻只读，线性查找开销可忽略）。
 */

#include "gateway_cap_registry.h"

#include "gateway_biz_internal.h"

#include "logging.h"
#include "airy_memory.h"

#include "gateway_hall_store.h"

#include <string.h>

/* 命名空间转发超时（ms）——与 02-l2-service-protocol.md 各 daemon 默认
 * 超时对齐（0.1.6 P1-4 收敛为单表，防散落定义漂移）。 */
static const struct {
    const char *ns;
    int timeout_ms;
} GW_NS_TIMEOUT[] = {
    {"llm", GW_LLM_DEFAULT_TIMEOUT_MS},
    {"think", GW_THINK_TIMEOUT_MS},
    {"agent", GW_TOOL_TIMEOUT_MS},
    {"tool", GW_TOOL_TIMEOUT_MS},
    {"a2a", GW_LLM_DEFAULT_TIMEOUT_MS},
    {"plugin", GW_TOOL_TIMEOUT_MS},
    {"info", GW_TOOL_TIMEOUT_MS},
    {"notify", GW_TOOL_TIMEOUT_MS},
    {"observe", GW_TOOL_TIMEOUT_MS},
    {"market", GW_TOOL_TIMEOUT_MS},
    {"hook", GW_TOOL_TIMEOUT_MS},
    {"sched", GW_TOOL_TIMEOUT_MS},
    {"monit", GW_TOOL_TIMEOUT_MS},
    {"channel", GW_TOOL_TIMEOUT_MS},
    {"cupolas", GW_TOOL_TIMEOUT_MS},
    {"mem", GW_TOOL_TIMEOUT_MS},
};

/* 统一能力注册表（SSoT）：cap_key 唯一，格式 "<ns>.<method>"。
 * 注意：特殊处理项（MEM/LLM_LIST/TOOL_APPROVE/HALL/AGENT_RUN）的
 * method 字段为网关内部处理子名，仅作人类可读登记；路由仍由
 * gateway_business_handler.c 的专用分支承担。 */
static const gw_cap_t GW_CAP_REGISTRY[] = {
    /* ── llm（转发 + list_models 特殊） ─────────────────────────── */
    {"llm.complete", "llm", "complete", GW_CAP_KIND_FWD},
    {"llm.list_models", "llm", "list_models", GW_CAP_KIND_LLM_LIST},
    {"llm.count_tokens", "llm", "count_tokens", GW_CAP_KIND_FWD},
    {"llm.health_check", "llm", "health_check", GW_CAP_KIND_FWD},
    {"llm.get_stats", "llm", "get_stats", GW_CAP_KIND_FWD},

    /* ── think（双思考） ────────────────────────────────────────── */
    {"think.process", "think", "process", GW_CAP_KIND_FWD},
    {"think.orchestrate", "think", "orchestrate", GW_CAP_KIND_FWD},
    {"think.health_check", "think", "health_check", GW_CAP_KIND_FWD},
    {"think.get_stats", "think", "get_stats", GW_CAP_KIND_FWD},

    /* ── agent（编排特殊 + 转发） ───────────────────────────────── */
    {"agent.run", "agent", "run", GW_CAP_KIND_AGENT_RUN},
    {"agent.cancel", "agent", "cancel", GW_CAP_KIND_AGENT_RUN},
    {"agent.spawn", "agent", "spawn", GW_CAP_KIND_FWD},
    {"agent.terminate", "agent", "terminate", GW_CAP_KIND_FWD},
    {"agent.invoke", "agent", "invoke", GW_CAP_KIND_FWD},
    {"agent.list", "agent", "list", GW_CAP_KIND_FWD},
    {"agent.count", "agent", "count", GW_CAP_KIND_FWD},
    {"agent.health_check", "agent", "health_check", GW_CAP_KIND_FWD},
    {"agent.get_stats", "agent", "get_stats", GW_CAP_KIND_FWD},

    /* ── tool（审批特殊 + 转发） ────────────────────────────────── */
    {"tool.pending", "tool", "pending", GW_CAP_KIND_TOOL_APPROVE},
    {"tool.approve", "tool", "approve", GW_CAP_KIND_TOOL_APPROVE},
    {"tool.register", "tool", "register", GW_CAP_KIND_FWD},
    {"tool.list_tools", "tool", "list_tools", GW_CAP_KIND_FWD},
    {"tool.get_tool", "tool", "get_tool", GW_CAP_KIND_FWD},
    {"tool.execute_tool", "tool", "execute_tool", GW_CAP_KIND_FWD},
    {"tool.execute", "tool", "execute", GW_CAP_KIND_FWD},
    {"tool.list", "tool", "list", GW_CAP_KIND_FWD},
    {"tool.health_check", "tool", "health_check", GW_CAP_KIND_FWD},
    {"tool.get_stats", "tool", "get_stats", GW_CAP_KIND_FWD},

    /* ── mem（特殊：AIRY_GATEWAY_MEM_PUBLIC 门控） ──────────────── */
    {"mem.write", "mem", "write", GW_CAP_KIND_MEM},
    {"mem.search", "mem", "search", GW_CAP_KIND_MEM},
    {"mem.get", "mem", "get", GW_CAP_KIND_MEM},
    {"mem.delete", "mem", "delete", GW_CAP_KIND_MEM},
    {"mem.count", "mem", "count", GW_CAP_KIND_MEM},
    {"mem.recent", "mem", "recent", GW_CAP_KIND_MEM},
    {"mem.evolve", "mem", "evolve", GW_CAP_KIND_MEM},
    {"mem.health_check", "mem", "health_check", GW_CAP_KIND_MEM},
    {"mem.get_stats", "mem", "get_stats", GW_CAP_KIND_MEM},
    {"mem.kb_ingest", "mem", "kb_ingest", GW_CAP_KIND_MEM},
    {"mem.kb_search", "mem", "kb_search", GW_CAP_KIND_MEM},
    {"mem.kb_delete", "mem", "kb_delete", GW_CAP_KIND_MEM},
    {"mem.kb_list", "mem", "kb_list", GW_CAP_KIND_MEM},

    /* ── a2a（Agent-to-Agent） ──────────────────────────────────── */
    {"a2a.register_agent", "a2a", "register_agent", GW_CAP_KIND_FWD},
    {"a2a.unregister_agent", "a2a", "unregister_agent", GW_CAP_KIND_FWD},
    {"a2a.discover_agents", "a2a", "discover_agents", GW_CAP_KIND_FWD},
    {"a2a.create_task", "a2a", "create_task", GW_CAP_KIND_FWD},
    {"a2a.update_task", "a2a", "update_task", GW_CAP_KIND_FWD},
    {"a2a.cancel_task", "a2a", "cancel_task", GW_CAP_KIND_FWD},
    {"a2a.get_task", "a2a", "get_task", GW_CAP_KIND_FWD},
    {"a2a.send_message", "a2a", "send_message", GW_CAP_KIND_FWD},
    {"a2a.count", "a2a", "count", GW_CAP_KIND_FWD},
    {"a2a.send", "a2a", "send", GW_CAP_KIND_FWD},
    {"a2a.receive", "a2a", "receive", GW_CAP_KIND_FWD},
    {"a2a.health_check", "a2a", "health_check", GW_CAP_KIND_FWD},
    {"a2a.get_stats", "a2a", "get_stats", GW_CAP_KIND_FWD},

    /* ── plugin ─────────────────────────────────────────────────── */
    {"plugin.load", "plugin", "load", GW_CAP_KIND_FWD},
    {"plugin.unload", "plugin", "unload", GW_CAP_KIND_FWD},
    {"plugin.start", "plugin", "start", GW_CAP_KIND_FWD},
    {"plugin.stop", "plugin", "stop", GW_CAP_KIND_FWD},
    {"plugin.execute", "plugin", "execute", GW_CAP_KIND_FWD},
    {"plugin.get_metadata", "plugin", "get_metadata", GW_CAP_KIND_FWD},
    {"plugin.get_state", "plugin", "get_state", GW_CAP_KIND_FWD},
    {"plugin.get_stats", "plugin", "get_stats", GW_CAP_KIND_FWD},
    {"plugin.list", "plugin", "list", GW_CAP_KIND_FWD},
    {"plugin.install", "plugin", "install", GW_CAP_KIND_FWD},
    {"plugin.uninstall", "plugin", "uninstall", GW_CAP_KIND_FWD},
    {"plugin.health_check", "plugin", "health_check", GW_CAP_KIND_FWD},

    /* ── info ───────────────────────────────────────────────────── */
    {"info.system", "info", "system", GW_CAP_KIND_FWD},
    {"info.history", "info", "history", GW_CAP_KIND_FWD},
    {"info.health", "info", "health", GW_CAP_KIND_FWD},
    {"info.health_check", "info", "health_check", GW_CAP_KIND_FWD},
    {"info.get_stats", "info", "get_stats", GW_CAP_KIND_FWD},

    /* ── notify ─────────────────────────────────────────────────── */
    {"notify.publish", "notify", "publish", GW_CAP_KIND_FWD},
    {"notify.subscribe", "notify", "subscribe", GW_CAP_KIND_FWD},
    {"notify.unsubscribe", "notify", "unsubscribe", GW_CAP_KIND_FWD},
    {"notify.list", "notify", "list", GW_CAP_KIND_FWD},
    {"notify.health", "notify", "health", GW_CAP_KIND_FWD},
    {"notify.health_check", "notify", "health_check", GW_CAP_KIND_FWD},
    {"notify.get_stats", "notify", "get_stats", GW_CAP_KIND_FWD},

    /* ── observe ────────────────────────────────────────────────── */
    {"observe.record_metric", "observe", "record_metric", GW_CAP_KIND_FWD},
    {"observe.query_metrics", "observe", "query_metrics", GW_CAP_KIND_FWD},
    {"observe.get_metrics", "observe", "get_metrics", GW_CAP_KIND_FWD},
    {"observe.get_stats", "observe", "get_stats", GW_CAP_KIND_FWD},
    {"observe.health_check", "observe", "health_check", GW_CAP_KIND_FWD},

    /* ── market ─────────────────────────────────────────────────── */
    {"market.register_agent", "market", "register_agent", GW_CAP_KIND_FWD},
    {"market.search_agents", "market", "search_agents", GW_CAP_KIND_FWD},
    {"market.install_agent", "market", "install_agent", GW_CAP_KIND_FWD},
    {"market.register_skill", "market", "register_skill", GW_CAP_KIND_FWD},
    {"market.search_skills", "market", "search_skills", GW_CAP_KIND_FWD},
    {"market.health_check", "market", "health_check", GW_CAP_KIND_FWD},
    {"market.publish", "market", "publish", GW_CAP_KIND_FWD},
    {"market.search", "market", "search", GW_CAP_KIND_FWD},
    {"market.install", "market", "install", GW_CAP_KIND_FWD},
    {"market.get_stats", "market", "get_stats", GW_CAP_KIND_FWD},

    /* ── hook ───────────────────────────────────────────────────── */
    {"hook.register", "hook", "register", GW_CAP_KIND_FWD},
    {"hook.unregister", "hook", "unregister", GW_CAP_KIND_FWD},
    {"hook.trigger", "hook", "trigger", GW_CAP_KIND_FWD},
    {"hook.list", "hook", "list", GW_CAP_KIND_FWD},
    {"hook.status", "hook", "status", GW_CAP_KIND_FWD},
    {"hook.stats", "hook", "stats", GW_CAP_KIND_FWD},
    {"hook.health", "hook", "health", GW_CAP_KIND_FWD},
    {"hook.ping", "hook", "ping", GW_CAP_KIND_FWD},
    {"hook.health_check", "hook", "health_check", GW_CAP_KIND_FWD},
    {"hook.get_stats", "hook", "get_stats", GW_CAP_KIND_FWD},

    /* ── sched ──────────────────────────────────────────────────── */
    {"sched.register_agent", "sched", "register_agent", GW_CAP_KIND_FWD},
    {"sched.unregister_agent", "sched", "unregister_agent", GW_CAP_KIND_FWD},
    {"sched.schedule_task", "sched", "schedule_task", GW_CAP_KIND_FWD},
    {"sched.get_task", "sched", "get_task", GW_CAP_KIND_FWD},
    {"sched.cancel", "sched", "cancel", GW_CAP_KIND_FWD},
    {"sched.dag_submit", "sched", "dag_submit", GW_CAP_KIND_FWD},
    {"sched.dag_status", "sched", "dag_status", GW_CAP_KIND_FWD},
    {"sched.dag_cancel", "sched", "dag_cancel", GW_CAP_KIND_FWD},
    {"sched.checkpoint_save", "sched", "checkpoint_save", GW_CAP_KIND_FWD},
    {"sched.submit", "sched", "submit", GW_CAP_KIND_FWD},
    {"sched.query", "sched", "query", GW_CAP_KIND_FWD},
    {"sched.get_stats", "sched", "get_stats", GW_CAP_KIND_FWD},
    {"sched.health_check", "sched", "health_check", GW_CAP_KIND_FWD},

    /* ── monit ──────────────────────────────────────────────────── */
    {"monit.record_metric", "monit", "record_metric", GW_CAP_KIND_FWD},
    {"monit.get_metrics", "monit", "get_metrics", GW_CAP_KIND_FWD},
    {"monit.trigger_alert", "monit", "trigger_alert", GW_CAP_KIND_FWD},
    {"monit.get_alerts", "monit", "get_alerts", GW_CAP_KIND_FWD},
    {"monit.health_check", "monit", "health_check", GW_CAP_KIND_FWD},
    {"monit.generate_report", "monit", "generate_report", GW_CAP_KIND_FWD},
    {"monit.heartbeat", "monit", "heartbeat", GW_CAP_KIND_FWD},
    {"monit.metrics", "monit", "metrics", GW_CAP_KIND_FWD},
    {"monit.alert_raise", "monit", "alert_raise", GW_CAP_KIND_FWD},
    {"monit.alert_resolve", "monit", "alert_resolve", GW_CAP_KIND_FWD},
    {"monit.get_stats", "monit", "get_stats", GW_CAP_KIND_FWD},

    /* ── channel ────────────────────────────────────────────────── */
    {"channel.ping", "channel", "ping", GW_CAP_KIND_FWD},
    {"channel.list", "channel", "list", GW_CAP_KIND_FWD},
    {"channel.open", "channel", "open", GW_CAP_KIND_FWD},
    {"channel.close", "channel", "close", GW_CAP_KIND_FWD},
    {"channel.send", "channel", "send", GW_CAP_KIND_FWD},
    {"channel.health", "channel", "health", GW_CAP_KIND_FWD},
    {"channel.health_check", "channel", "health_check", GW_CAP_KIND_FWD},
    {"channel.get_stats", "channel", "get_stats", GW_CAP_KIND_FWD},

    /* ── cupolas ────────────────────────────────────────────────── */
    {"cupolas.check_permission", "cupolas", "check_permission", GW_CAP_KIND_FWD},
    {"cupolas.sanitize", "cupolas", "sanitize", GW_CAP_KIND_FWD},
    {"cupolas.execute_command", "cupolas", "execute_command", GW_CAP_KIND_FWD},
    {"cupolas.add_rule", "cupolas", "add_rule", GW_CAP_KIND_FWD},
    {"cupolas.audit_flush", "cupolas", "audit_flush", GW_CAP_KIND_FWD},
    {"cupolas.health_check", "cupolas", "health_check", GW_CAP_KIND_FWD},
    {"cupolas.get_stats", "cupolas", "get_stats", GW_CAP_KIND_FWD},
    {"cupolas.vault_store", "cupolas", "vault_store", GW_CAP_KIND_FWD},
    {"cupolas.vault_retrieve", "cupolas", "vault_retrieve", GW_CAP_KIND_FWD},
    {"cupolas.vault_delete", "cupolas", "vault_delete", GW_CAP_KIND_FWD},
    {"cupolas.vault_list", "cupolas", "vault_list", GW_CAP_KIND_FWD},
    {"cupolas.vault_rotate", "cupolas", "vault_rotate", GW_CAP_KIND_FWD},
    {"cupolas.net_add_rule", "cupolas", "net_add_rule", GW_CAP_KIND_FWD},
    {"cupolas.net_check_access", "cupolas", "net_check_access", GW_CAP_KIND_FWD},
    {"cupolas.net_get_stats", "cupolas", "net_get_stats", GW_CAP_KIND_FWD},
    {"cupolas.entitlements_load", "cupolas", "entitlements_load", GW_CAP_KIND_FWD},
    {"cupolas.entitlements_check", "cupolas", "entitlements_check", GW_CAP_KIND_FWD},

    /* ── hall（网关内实现） ─────────────────────────────────────── */
    {"hall.board", "hall", "board", GW_CAP_KIND_HALL},
    {"hall.tasks", "hall", "tasks", GW_CAP_KIND_HALL},
    {"hall.replay", "hall", "replay", GW_CAP_KIND_HALL},
    {"hall.stream", "hall", "stream", GW_CAP_KIND_HALL},
};

#define GW_CAP_COUNT (sizeof(GW_CAP_REGISTRY) / sizeof(GW_CAP_REGISTRY[0]))

const gw_cap_t *gw_cap_find(const char *cap_key)
{
    if (!cap_key)
        return NULL;
    for (size_t i = 0; i < GW_CAP_COUNT; ++i) {
        if (strcmp(GW_CAP_REGISTRY[i].cap_key, cap_key) == 0)
            return &GW_CAP_REGISTRY[i];
    }
    return NULL;
}

size_t gw_cap_count(void)
{
    return GW_CAP_COUNT;
}

const gw_cap_t *gw_cap_at(size_t index)
{
    if (index >= GW_CAP_COUNT)
        return NULL;
    return &GW_CAP_REGISTRY[index];
}

int gw_cap_ns_timeout(const char *ns)
{
    if (!ns)
        return GW_TOOL_TIMEOUT_MS;
    for (size_t i = 0; i < sizeof(GW_NS_TIMEOUT) / sizeof(GW_NS_TIMEOUT[0]); ++i) {
        if (strcmp(GW_NS_TIMEOUT[i].ns, ns) == 0)
            return GW_NS_TIMEOUT[i].timeout_ms;
    }
    return GW_TOOL_TIMEOUT_MS;
}

void gw_cap_emit(const char *cap_key, const char *status, const char *detail)
{
    if (!cap_key || !status)
        return;
    cJSON *content = cJSON_CreateObject();
    if (!content)
        return;
    cJSON_AddStringToObject(content, "cap_key", cap_key);
    cJSON_AddStringToObject(content, "status", status);
    if (detail && *detail)
        cJSON_AddStringToObject(content, "detail", detail);
    char *content_str = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_str)
        return;
    /* 事件流单一真相源：cap 类别事件（task_id 用 "gateway" 汇聚网关侧能力调用） */
    (void)gw_hall_store_event("gateway", "cap", NULL, content_str);
    AIRY_FREE(content_str);
}
