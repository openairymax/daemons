// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_rpc_policy.c
 * @brief cupolas_d policy.* 命名空间 RPC：策略加载 / 激活 / 回滚 / 状态。
 *
 * M2-S3（0.1.9 §3.2 PDP）：cupolas_d 作为唯一策略持有者（PDP），
 * 通过 dynamic_policy_engine 暴露运行时策略演化接口（两段式生效）：
 *   - policy.load     ：装载策略集入暂存（不改变运行裁决，epoch 不变）
 *                        + 冲突检测报告（针对暂存文档）
 *   - policy.activate ：暂存集原子提交运行集 + 版本固化 + epoch+1
 *                        （热更新唯一生效点，触发 PEP 缓存失效广播）
 *   - policy.rollback ：回滚到历史版本，epoch+1（秒级回退，32 版历史）
 *   - policy.status   ：epoch/版本数/运行规则数/暂存数/冲突策略快照
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

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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

/* M2-S4 (0.1.9 §3.2)：epoch 变更广播至 notify_d topic=airy.cupolas.epoch。
 * 广播为尽力而为通知面（fail-open）：notify_d 不在线/失败仅告警，
 * 不阻断策略已生效的事实（PEP 下次调用仍会经 policy_status 对齐 epoch）。 */
static void broadcast_epoch(uint64_t epoch)
{
    const char *sp = airy_runtime_dir_socket("notify.sock");
    if (!sp || !sp[0]) {
        SVC_LOG_WARN("policy: notify socket path unavailable, epoch broadcast skipped");
        return;
    }

    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"publish\",\"params\":{"
             "\"topic\":\"airy.cupolas.epoch\",\"event\":\"epoch_change\","
             "\"payload\":\"{\\\"epoch\\\":%llu}\"}}",
             (unsigned long long)epoch);

#ifdef _WIN32
    /* Windows 侧 notify_d 未启用命名管道；广播降级为日志（尽力而为） */
    SVC_LOG_INFO("policy: epoch=%llu broadcast logged (win32 no-op)",
                 (unsigned long long)epoch);
    (void)sp;
    (void)req;
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SVC_LOG_WARN("policy: socket() failed, epoch broadcast skipped");
        return;
    }
    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sp);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        SVC_LOG_WARN("policy: notify_d unreachable, epoch broadcast skipped");
        close(fd);
        return;
    }
    (void)send(fd, req, strlen(req), 0);
    char buf[1024];
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            SVC_LOG_INFO("policy: epoch=%llu broadcast ack: %.120s",
                         (unsigned long long)epoch, buf);
        }
    }
    close(fd);
#endif
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

    /* M2-S2 两段式生效（§3.3.1）：load 仅装载入暂存集——不改变运行裁决，
     * epoch 不变；冲突报告针对暂存文档；客户端据报告决定是否 activate。 */
    int rc = dpolicy_engine_stage_json(g_dpolicy, json);
    if (rc == -1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Policy document is not valid JSON", id);
        return;
    }
    if (rc != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Policy document rejected (missing rules array or invalid rule)", id);
        return;
    }

    dpolicy_conflict_t *conflicts = NULL;
    size_t n = 0;
    dpolicy_engine_detect_staged_conflicts(g_dpolicy, &conflicts, &n);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "staged", true);
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_staged_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "conflict_count", (double)n);
    cJSON_AddStringToObject(result, "strategy",
                            strategy_str(dpolicy_engine_get_strategy(g_dpolicy)));
    /* 暂存不生效：epoch 保持当前运行版本（客户端可对比 activate 前） */
    cJSON_AddNumberToObject(result, "epoch",
                            (double)dpolicy_engine_get_epoch(g_dpolicy));
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

    SVC_LOG_INFO("policy.load: %zu rules staged (activate to apply), %zu conflicts",
                 dpolicy_engine_get_staged_count(g_dpolicy), n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_policy_activate(cJSON *params, int id, airy_sock_t client_fd)
{
    if (require_engine(client_fd, id) != 0)
        return;

    const char *desc = get_string_field(params, "description", NULL);
    /* M2-S2：activate = 暂存集提交运行集 + 版本固化 + epoch+1 + 广播
     * （热更新唯一生效点，epoch SSoT 单调推进触发 PEP 缓存失效）。 */
    int rc = dpolicy_engine_activate(g_dpolicy, desc ? desc : "");
    if (rc == -5) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "No staged policy document (call policy.load first)", id);
        return;
    }
    if (rc != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Policy activation failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "epoch",
                            (double)dpolicy_engine_get_epoch(g_dpolicy));
    cJSON_AddNumberToObject(result, "version_count",
                            (double)dpolicy_engine_get_version_count(g_dpolicy));
    cJSON_AddNumberToObject(result, "rule_count",
                            (double)dpolicy_engine_get_rule_count(g_dpolicy));

    SVC_LOG_INFO("policy.activate: staged set applied, epoch=%llu rules=%zu",
                 (unsigned long long)dpolicy_engine_get_epoch(g_dpolicy),
                 dpolicy_engine_get_rule_count(g_dpolicy));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    broadcast_epoch(dpolicy_engine_get_epoch(g_dpolicy));
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
    broadcast_epoch(dpolicy_engine_get_epoch(g_dpolicy));
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
    /* 暂存状态：load 后未 activate 的候选集（不参与运行裁决） */
    cJSON_AddBoolToObject(result, "staged",
                          dpolicy_engine_has_staged(g_dpolicy) ? true : false);
    cJSON_AddNumberToObject(result, "staged_rule_count",
                            (double)dpolicy_engine_get_staged_count(g_dpolicy));
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
