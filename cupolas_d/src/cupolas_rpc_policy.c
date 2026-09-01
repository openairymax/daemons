// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_rpc_policy.c
 * @brief cupolas_d policy.* 命名空间 RPC：策略加载 / 激活 / 回滚 / 状态。
 *
 * M2-S3（0.1.9 §3.2 PDP）：cupolas_d 作为唯一策略持有者（PDP），
 * 通过 dynamic_policy_engine 暴露运行时策略演化接口：
 *   - policy.load     ：加载策略集（整体替换幂等）+ 冲突检测报告
 *   - policy.activate ：commit 当前策略为版本，epoch+1（热更新生效点）
 *   - policy.rollback ：回滚到历史版本，epoch+1（秒级回退，32 版历史）
 *   - policy.status   ：epoch/版本数/规则数/冲突策略快照
 *
 * epoch 单调递增为引擎 SSoT，M2-S4 广播经 notify_d 下发，
 * M2-S5 作为各 daemon PEP 缓存失效键。
 */

#include "airy_memory.h"
#include "error.h"
#include "cupolas_d_internal.h"

#include "daemon_main.h"
#include "param_validator.h"
#include "svc_logger.h"

#include "dynamic_policy_engine.h"

static const char *strategy_str(dpolicy_conflict_strategy_t s)
{
    switch (s) {
    case DPOLICY_CONFLICT_ALLOW_WINS:
        return "allow_wins";
    case DPOLICY_CONFLICT_HIGHEST_PRIORITY:
        return "highest_priority";
    case DPOLICY_CONFLICT_MOST_RESTRICTIVE:
        return "most_restrictive";
    case DPOLICY_CONFLICT_DENY_WINS:
    default:
        return "deny_wins";
    }
}

static int require_engine(airy_sock_t client_fd, int id)
{
    if (g_dpolicy)
        return 0;
    JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                       "Dynamic policy engine not ready", id);
    return -1;
}

static void handle_policy_load(cJSON *params, int id, airy_sock_t client_fd)
{
    if (require_engine(client_fd, id) != 0)
        return;

    const char *json = get_string_field(params, "json", NULL);
    if (!json || !json[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "json (policy document) required", id);
        return;
    }

    int rc = dpolicy_engine_load_policies_json(g_dpolicy, json);
    if (rc != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Policy document rejected", id);
        return;
    }

    /* 冲突检测报告：load 不自动生效，客户端据冲突数决定是否 activate */
    dpolicy_conflict_t *conflicts = NULL;
    size_t n = 0;
    dpolicy_engine_detect_conflicts(g_dpolicy, &conflicts, &n);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_rule_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "conflict_count", (double)n);
    cJSON_AddStringToObject(result, "strategy",
                            strategy_str(dpolicy_engine_get_strategy(g_dpolicy)));
    if (conflicts) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < n; i++) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "rule_a", conflicts[i].rule_a_id);
            cJSON_AddStringToObject(c, "rule_b", conflicts[i].rule_b_id);
            cJSON_AddStringToObject(c, "reason", conflicts[i].reason);
            cJSON_AddItemToArray(arr, c);
        }
        cJSON_AddItemToObject(result, "conflicts", arr);
    }
    AIRY_FREE(conflicts);

    SVC_LOG_INFO("policy.load: %zu rules, %zu conflicts",
                 dpolicy_engine_get_rule_count(g_dpolicy), n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_policy_activate(cJSON *params, int id, airy_sock_t client_fd)
{
    if (require_engine(client_fd, id) != 0)
        return;

    const char *desc = get_string_field(params, "description", NULL);
    int rc = dpolicy_engine_commit_version(g_dpolicy, desc ? desc : "");
    if (rc != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Policy commit failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "epoch",
                            (double)dpolicy_engine_get_epoch(g_dpolicy));
    cJSON_AddNumberToObject(result, "version_count",
                            (double)dpolicy_engine_get_version_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_rule_count(g_dpolicy));

    SVC_LOG_INFO("policy.activate: epoch=%llu",
                 (unsigned long long)dpolicy_engine_get_epoch(g_dpolicy));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_policy_rollback(cJSON *params, int id, airy_sock_t client_fd)
{
    if (require_engine(client_fd, id) != 0)
        return;

    const char *version = get_string_field(params, "version", NULL);
    if (!version || !version[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "version required", id);
        return;
    }

    int rc = dpolicy_engine_rollback(g_dpolicy, version);
    if (rc == -2) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Version not found in history", id);
        return;
    }
    if (rc != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Policy rollback failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "epoch",
                            (double)dpolicy_engine_get_epoch(g_dpolicy));
    cJSON_AddNumberToObject(result, "version_count",
                            (double)dpolicy_engine_get_version_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_rule_count(g_dpolicy));

    SVC_LOG_INFO("policy.rollback: %s, epoch=%llu", version,
                 (unsigned long long)dpolicy_engine_get_epoch(g_dpolicy));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_policy_status(cJSON *params __attribute__((unused)), int id,
                                 airy_sock_t client_fd)
{
    if (require_engine(client_fd, id) != 0)
        return;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "enabled", true);
    cJSON_AddNumberToObject(result, "epoch",
                            (double)dpolicy_engine_get_epoch(g_dpolicy));
    cJSON_AddNumberToObject(result, "version_count",
                            (double)dpolicy_engine_get_version_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_rule_count(g_dpolicy));
    cJSON_AddStringToObject(result, "strategy",
                            strategy_str(dpolicy_engine_get_strategy(g_dpolicy)));
    cJSON_AddNumberToObject(result, "max_versions", DPOLICY_MAX_VERSIONS);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

void on_policy_load_method(cJSON *params, int id, void *user_data)
{
    handle_policy_load(params, id, *(airy_sock_t *)user_data);
}

void on_policy_activate_method(cJSON *params, int id, void *user_data)
{
    handle_policy_activate(params, id, *(airy_sock_t *)user_data);
}

void on_policy_rollback_method(cJSON *params, int id, void *user_data)
{
    handle_policy_rollback(params, id, *(airy_sock_t *)user_data);
}

void on_policy_status_method(cJSON *params, int id, void *user_data)
{
    handle_policy_status(params, id, *(airy_sock_t *)user_data);
}
