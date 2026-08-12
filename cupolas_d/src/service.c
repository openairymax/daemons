// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 *
 * @file service.c
 * @brief Cupolas 安全穹顶服务实现：权限裁决/输入净化/命令执行/规则管理/审计
 *
 * cupolas_d 将 cupolas 安全库（agentrt/cupolas）封装为独立 daemon 服务。
 * 本服务真实调用 cupolas.h 公共 API（IRON-2：所有方法无桩）：
 *   - cupolas_check_permission / cupolas_add_permission_rule
 *   - cupolas_sanitize_input / cupolas_execute_command
 *   - cupolas_flush_audit_log / cupolas_version
 *
 * 设计要点：
 * - cupolas 为进程级单例库（cupolas_init），模块初始化由 main() 通过
 *   daemon_cupolas_init("cupolas_d") 完成；本服务实例仅承载配置元数据与
 *   真实运行统计（原子计数器：权限检查次数 / 净化次数）。
 * - 统计为真实计数：每次 check_permission / sanitize 调用都会递增，
 *   供 cupolas.get_stats 返回。
 */

#include "cupolas_service.h"

#include "cupolas.h"
#include "cupolas_entitlements.h"
#include "cupolas_error.h"
#include "cupolas_network_security.h"
#include "cupolas_vault.h"
#include "daemon_security.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUPOLAS_EXEC_OUTPUT_SIZE (64 * 1024)

#define CUPOLAS_SANITIZE_MAX_OUTPUT (1024 * 1024)

struct cupolas_service {
    char *config_path;
    int64_t start_time;
    atomic_uint_fast64_t permission_checks;
    atomic_uint_fast64_t sanitize_count;
    cupolas_entitlements_t *entitlements;
    char *entitlements_path;
};

cupolas_service_t *cupolas_service_create(const char *config_path)
{
    cupolas_service_t *svc = AIRY_CALLOC(1, sizeof(cupolas_service_t));
    if (!svc)
        return NULL;

    svc->config_path = config_path ? AIRY_STRDUP(config_path) : NULL;
    svc->start_time = (int64_t)time(NULL);
    atomic_init(&svc->permission_checks, 0);
    atomic_init(&svc->sanitize_count, 0);
    return svc;
}

void cupolas_service_destroy(cupolas_service_t *svc)
{
    if (!svc)
        return;
    cupolas_entitlements_free(svc->entitlements);
    svc->entitlements = NULL;
    AIRY_FREE(svc->entitlements_path);
    AIRY_FREE(svc->config_path);
    AIRY_FREE(svc);
}

int cupolas_service_check_permission(cupolas_service_t *svc,
                                     const cupolas_check_permission_params_t *params,
                                     cupolas_check_permission_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->agent_id || !params->action || !params->resource)
        return AIRY_ERR_INVALID_PARAM;

    atomic_fetch_add_explicit(&svc->permission_checks, 1, memory_order_relaxed);

    int ret = cupolas_check_permission(params->agent_id, params->action, params->resource,
                                       params->context);
    if (ret < 0) {
        SVC_LOG_ERROR("cupolas.check_permission failed: agent=%s action=%s resource=%s rc=%d",
                      params->agent_id, params->action, params->resource, ret);
        return ret;
    }

    out->allowed = (ret > 0) ? 1 : 0;
    out->err = ret;
    SVC_LOG_INFO("cupolas.check_permission: agent=%s action=%s resource=%s -> %s", params->agent_id,
                 params->action, params->resource, out->allowed ? "allowed" : "denied");
    return AIRY_SUCCESS;
}

int cupolas_service_sanitize(cupolas_service_t *svc, const cupolas_sanitize_params_t *params,
                             cupolas_sanitize_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->input)
        return AIRY_ERR_INVALID_PARAM;

    size_t in_len = strlen(params->input);

    size_t out_size = in_len * 6 + 64;
    if (out_size < 256)
        out_size = 256;
    if (out_size > CUPOLAS_SANITIZE_MAX_OUTPUT)
        out_size = CUPOLAS_SANITIZE_MAX_OUTPUT;

    char *buf = AIRY_MALLOC(out_size);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;

    int ret = cupolas_sanitize_input(params->input, buf, out_size);

    /* In strict mode, dangerous input is rejected (SANITIZE_REJECTED/ERROR
     * or guard interception): the output buffer stays empty and a non-zero
     * code is returned — no sanitized artifact, treated as denied
     * (fail-closed). */
    if (ret != CUPOLAS_OK && buf[0] == '\0') {
        SVC_LOG_WARN("cupolas.sanitize: input rejected (rc=%d)", ret);
        AIRY_FREE(buf);
        return AIRY_ERR_PERMISSION_DENIED;
    }

    atomic_fetch_add_explicit(&svc->sanitize_count, 1, memory_order_relaxed);
    out->sanitized = buf;
    out->err = ret;
    SVC_LOG_INFO("cupolas.sanitize: input_len=%zu rc=%d", in_len, ret);
    return AIRY_SUCCESS;
}

void cupolas_sanitize_result_free(cupolas_sanitize_result_t *out)
{
    if (!out)
        return;
    AIRY_FREE(out->sanitized);
    out->sanitized = NULL;
    out->err = 0;
}

int cupolas_service_execute_command(cupolas_service_t *svc,
                                    const cupolas_execute_command_params_t *params,
                                    cupolas_execute_command_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->command || !params->argv)
        return AIRY_ERR_INVALID_PARAM;

    char *stdout_buf = AIRY_CALLOC(1, CUPOLAS_EXEC_OUTPUT_SIZE);
    char *stderr_buf = AIRY_CALLOC(1, CUPOLAS_EXEC_OUTPUT_SIZE);
    if (!stdout_buf || !stderr_buf) {
        AIRY_FREE(stdout_buf);
        AIRY_FREE(stderr_buf);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int exit_code = -1;
    int ret =
        cupolas_execute_command(params->command, params->argv, &exit_code, stdout_buf,
                                CUPOLAS_EXEC_OUTPUT_SIZE, stderr_buf, CUPOLAS_EXEC_OUTPUT_SIZE);
    if (ret != CUPOLAS_OK) {
        AIRY_FREE(stdout_buf);
        AIRY_FREE(stderr_buf);
        SVC_LOG_ERROR("cupolas.execute_command failed: cmd=%s rc=%d", params->command, ret);
        return ret;
    }

    out->exit_code = exit_code;
    out->stdout_buf = stdout_buf;
    out->stderr_buf = stderr_buf;
    out->err = 0;
    SVC_LOG_INFO("cupolas.execute_command: cmd=%s exit_code=%d", params->command, exit_code);
    return AIRY_SUCCESS;
}

void cupolas_execute_command_result_free(cupolas_execute_command_result_t *out)
{
    if (!out)
        return;
    AIRY_FREE(out->stdout_buf);
    AIRY_FREE(out->stderr_buf);
    out->stdout_buf = NULL;
    out->stderr_buf = NULL;
    out->exit_code = -1;
    out->err = 0;
}

int cupolas_service_add_rule(cupolas_service_t *svc, const cupolas_add_rule_params_t *params,
                             cupolas_add_rule_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->resource)
        return AIRY_ERR_INVALID_PARAM;

    int ret = cupolas_add_permission_rule(params->agent_id, params->action, params->resource,
                                          params->allow, params->priority);
    if (ret != CUPOLAS_OK) {
        SVC_LOG_ERROR("cupolas.add_rule failed: resource=%s rc=%d", params->resource, ret);
        return ret;
    }

    out->added = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.add_rule: agent=%s action=%s resource=%s allow=%d priority=%d",
                 params->agent_id ? params->agent_id : "*", params->action ? params->action : "*",
                 params->resource, params->allow, params->priority);
    return AIRY_SUCCESS;
}

int cupolas_service_audit_flush(cupolas_service_t *svc)
{
    if (!svc)
        return AIRY_ERR_INVALID_PARAM;
    cupolas_flush_audit_log();
    SVC_LOG_INFO("cupolas.audit_flush: audit log flushed");
    return AIRY_SUCCESS;
}

char *cupolas_service_get_stats_json(cupolas_service_t *svc)
{
    if (!svc)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    cJSON_AddStringToObject(obj, "daemon", "cupolas_d");
    cJSON_AddStringToObject(obj, "version", cupolas_version());
    cJSON_AddNumberToObject(obj, "uptime_s", (double)((int64_t)time(NULL) - svc->start_time));
    cJSON_AddNumberToObject(obj, "permission_checks",
                            (double)atomic_load_explicit(&svc->permission_checks,
                                                         memory_order_relaxed));
    cJSON_AddNumberToObject(obj, "sanitize_count",
                            (double)atomic_load_explicit(&svc->sanitize_count,
                                                         memory_order_relaxed));

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/* The vault instance is opened by daemon_security (daemon_cupolas_init ->
 * daemon_security_init); this service reuses the same instance via
 * daemon_security_get_vault(), consistent with
 * daemon_store/retrieve_credential reads and writes. */

int cupolas_service_vault_store(cupolas_service_t *svc, const cupolas_vault_store_params_t *params,
                                cupolas_vault_store_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->cred_id || !params->data || params->data_len == 0)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_vault_t *vault = daemon_security_get_vault();
    if (!vault)
        return AIRY_ERR_STATE_ERROR;

    int rc =
        cupolas_vault_store(vault, params->cred_id, (cupolas_vault_cred_type_t)params->cred_type,
                            params->data, params->data_len, NULL);
    if (rc != 0) {
        SVC_LOG_ERROR("cupolas.vault_store failed: cred_id=%s rc=%d", params->cred_id, rc);
        out->stored = 0;
        out->err = rc;
        return AIRY_ERR_UNKNOWN;
    }

    const char *owner = params->agent_id ? params->agent_id : "system";
    (void)cupolas_vault_grant_access(vault, params->cred_id, owner,
                                     CUPOLAS_VAULT_OP_READ | CUPOLAS_VAULT_OP_WRITE |
                                         CUPOLAS_VAULT_OP_DELETE,
                                     0);

    out->stored = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.vault_store: cred_id=%s type=%d len=%zu owner=%s", params->cred_id,
                 params->cred_type, params->data_len, owner);
    return AIRY_SUCCESS;
}

int cupolas_service_vault_retrieve(cupolas_service_t *svc,
                                   const cupolas_vault_retrieve_params_t *params,
                                   cupolas_vault_retrieve_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->cred_id)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_vault_t *vault = daemon_security_get_vault();
    if (!vault)
        return AIRY_ERR_STATE_ERROR;

    const char *requester = params->agent_id ? params->agent_id : "system";

    /* The vault does not support NULL data_out to probe the length: first
     * call with a default buffer; if BUFFER_TOO_SMALL is returned, grow
     * according to the length the vault reports and retry. */
    size_t buf_len = 4096;
    uint8_t *buf = AIRY_MALLOC(buf_len);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;

    int rc = cupolas_vault_retrieve(vault, params->cred_id, requester, buf, &buf_len);
    if (rc == (int)cupolas_ERR_BUFFER_TOO_SMALL) {
        uint8_t *grown = AIRY_REALLOC(buf, buf_len);
        if (!grown) {
            AIRY_FREE(buf);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        buf = grown;
        rc = cupolas_vault_retrieve(vault, params->cred_id, requester, buf, &buf_len);
    }
    if (rc != 0) {
        AIRY_FREE(buf);
        SVC_LOG_WARN("cupolas.vault_retrieve failed: cred_id=%s agent=%s rc=%d", params->cred_id,
                     requester, rc);
        out->data = NULL;
        out->data_len = 0;
        out->err = rc;
        return AIRY_ERR_PERMISSION_DENIED;
    }

    out->data = buf;
    out->data_len = buf_len;
    out->err = 0;
    SVC_LOG_INFO("cupolas.vault_retrieve: cred_id=%s agent=%s len=%zu", params->cred_id, requester,
                 buf_len);
    return AIRY_SUCCESS;
}

void cupolas_vault_retrieve_result_free(cupolas_vault_retrieve_result_t *out)
{
    if (!out)
        return;
    AIRY_FREE(out->data);
    out->data = NULL;
    out->data_len = 0;
    out->err = 0;
}

int cupolas_service_vault_delete(cupolas_service_t *svc,
                                 const cupolas_vault_delete_params_t *params,
                                 cupolas_vault_delete_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->cred_id)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_vault_t *vault = daemon_security_get_vault();
    if (!vault)
        return AIRY_ERR_STATE_ERROR;

    const char *requester = params->agent_id ? params->agent_id : "system";
    int rc = cupolas_vault_delete(vault, params->cred_id, requester);
    if (rc != 0) {
        SVC_LOG_WARN("cupolas.vault_delete failed: cred_id=%s agent=%s rc=%d", params->cred_id,
                     requester, rc);
        out->deleted = 0;
        out->err = rc;
        return AIRY_ERR_UNKNOWN;
    }

    out->deleted = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.vault_delete: cred_id=%s agent=%s", params->cred_id, requester);
    return AIRY_SUCCESS;
}

char *cupolas_service_vault_list_json(cupolas_service_t *svc, int cred_type)
{
    if (!svc)
        return NULL;

    cupolas_vault_t *vault = daemon_security_get_vault();
    if (!vault)
        return NULL;

    cupolas_vault_metadata_t *metadata = NULL;
    size_t count = 0;
    int rc = cupolas_vault_list(vault, (cupolas_vault_cred_type_t)cred_type, &metadata, &count);
    if (rc != 0)
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        cupolas_vault_free_list(metadata, count);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "cred_id", metadata[i].cred_id ? metadata[i].cred_id : "");
        cJSON_AddNumberToObject(item, "type", (double)metadata[i].type);
        cJSON_AddStringToObject(item, "service", metadata[i].service ? metadata[i].service : "");
        cJSON_AddStringToObject(item, "account", metadata[i].account ? metadata[i].account : "");
        cJSON_AddNumberToObject(item, "created_at", (double)(uint64_t)metadata[i].created_at);
        cJSON_AddNumberToObject(item, "updated_at", (double)(uint64_t)metadata[i].updated_at);
        cJSON_AddItemToArray(arr, item);
    }
    cupolas_vault_free_list(metadata, count);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    SVC_LOG_INFO("cupolas.vault_list: type=%d count=%zu", cred_type, count);
    return json;
}

int cupolas_service_vault_rotate(cupolas_service_t *svc,
                                 const cupolas_vault_rotate_params_t *params,
                                 cupolas_vault_rotate_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->cred_group || params->strategy < CUPOLAS_VAULT_ROTATE_ROUND_ROBIN ||
        params->strategy > CUPOLAS_VAULT_ROTATE_PRIORITY)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_vault_t *vault = daemon_security_get_vault();
    if (!vault)
        return AIRY_ERR_STATE_ERROR;

    char selected_id[256];
    int rc = cupolas_vault_rotate_credential(vault, params->cred_group,
                                             (cupolas_vault_rotation_strategy_t)params->strategy,
                                             selected_id, sizeof(selected_id));
    if (rc != 0) {
        SVC_LOG_WARN("cupolas.vault_rotate failed: group=%s strategy=%d rc=%d", params->cred_group,
                     params->strategy, rc);
        out->selected_id = NULL;
        out->err = rc;
        return AIRY_ERR_UNKNOWN;
    }

    out->selected_id = AIRY_STRDUP(selected_id);
    out->err = 0;
    SVC_LOG_INFO("cupolas.vault_rotate: group=%s strategy=%d selected=%s", params->cred_group,
                 params->strategy, selected_id);
    return AIRY_SUCCESS;
}

static int parse_port_range(const char *port_str, uint16_t *start, uint16_t *end)
{
    if (!port_str || !*port_str || !start || !end)
        return -1;
    long p1 = strtol(port_str, NULL, 10);
    if (p1 < 0 || p1 > 65535)
        return -1;
    const char *dash = strchr(port_str, '-');
    if (dash) {
        long p2 = strtol(dash + 1, NULL, 10);
        if (p2 < 0 || p2 > 65535 || p2 < p1)
            return -1;
        *start = (uint16_t)p1;
        *end = (uint16_t)p2;
    } else {
        *start = (uint16_t)p1;
        *end = (uint16_t)p1;
    }
    return 0;
}

int cupolas_service_net_add_rule(cupolas_service_t *svc,
                                 const cupolas_net_add_rule_params_t *params,
                                 cupolas_net_add_rule_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->rule_id)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_net_filter_rule_t rule;
    __builtin_memset(&rule, 0, sizeof(rule));
    rule.rule_id = (char *)params->rule_id;
    rule.description = (char *)(params->description ? params->description : "");
    rule.src_ip_pattern = (char *)(params->src_ip ? params->src_ip : "*");
    rule.dst_ip_pattern = (char *)(params->dst_ip ? params->dst_ip : "*");
    rule.host_pattern = NULL;
    rule.url_pattern = NULL;
    rule.protocol = (cupolas_proto_t)params->protocol;
    rule.direction = (cupolas_direction_t)params->direction;
    rule.action = (cupolas_fw_action_t)params->action;
    rule.priority = params->priority;
    rule.enabled = true;

    if (parse_port_range(params->src_port, &rule.src_port_start, &rule.src_port_end) != 0 &&
        params->src_port) {
        SVC_LOG_WARN("cupolas.net_add_rule: invalid src_port '%s'", params->src_port);
        out->added = 0;
        out->err = AIRY_ERR_INVALID_PARAM;
        return AIRY_ERR_INVALID_PARAM;
    }
    if (parse_port_range(params->dst_port, &rule.dst_port_start, &rule.dst_port_end) != 0 &&
        params->dst_port) {
        SVC_LOG_WARN("cupolas.net_add_rule: invalid dst_port '%s'", params->dst_port);
        out->added = 0;
        out->err = AIRY_ERR_INVALID_PARAM;
        return AIRY_ERR_INVALID_PARAM;
    }

    int rc = cupolas_net_add_rule(&rule);
    if (rc != 0) {
        SVC_LOG_ERROR("cupolas.net_add_rule failed: rule_id=%s rc=%d", params->rule_id, rc);
        out->added = 0;
        out->err = rc;
        return AIRY_ERR_UNKNOWN;
    }

    out->added = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.net_add_rule: rule_id=%s proto=%d dir=%d action=%d", params->rule_id,
                 params->protocol, params->direction, params->action);
    return AIRY_SUCCESS;
}

int cupolas_service_net_check_access(cupolas_service_t *svc,
                                     const cupolas_net_check_access_params_t *params,
                                     cupolas_net_check_access_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->host)
        return AIRY_ERR_INVALID_PARAM;

    const char *direction = params->direction ? params->direction : "outbound";
    int rc = cupolas_net_check_access(params->host, params->port, (cupolas_proto_t)params->protocol,
                                      direction);
    if (rc == 0) {
        out->allowed = 1;
    } else if (rc < 0) {

        out->allowed = 0;
        out->err = rc;
        return AIRY_ERR_PERMISSION_DENIED;
    } else {
        out->allowed = 0;
    }
    out->err = 0;
    SVC_LOG_INFO("cupolas.net_check_access: host=%s port=%u proto=%d dir=%s -> %s", params->host,
                 params->port, params->protocol, direction, out->allowed ? "allowed" : "denied");
    return AIRY_SUCCESS;
}

char *cupolas_service_net_get_stats_json(cupolas_service_t *svc)
{
    if (!svc)
        return NULL;

    cupolas_net_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    int rc = cupolas_net_get_stats(&stats);
    if (rc != 0)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;
    cJSON_AddNumberToObject(obj, "total_connections", (double)stats.total_connections);
    cJSON_AddNumberToObject(obj, "active_connections", (double)stats.active_connections);
    cJSON_AddNumberToObject(obj, "tls_handshakes", (double)stats.tls_handshakes);
    cJSON_AddNumberToObject(obj, "tls_failures", (double)stats.tls_failures);
    cJSON_AddNumberToObject(obj, "firewall_blocks", (double)stats.firewall_blocks);
    cJSON_AddNumberToObject(obj, "rate_limit_hits", (double)stats.rate_limit_hits);
    cJSON_AddNumberToObject(obj, "cert_errors", (double)stats.cert_errors);
    cJSON_AddNumberToObject(obj, "hostname_mismatches", (double)stats.hostname_mismatches);
    cJSON_AddNumberToObject(obj, "dns_queries", (double)stats.dns_queries);
    cJSON_AddNumberToObject(obj, "dns_blocked", (double)stats.dns_blocked);
    cJSON_AddNumberToObject(obj, "http_requests", (double)stats.http_requests);
    cJSON_AddNumberToObject(obj, "https_requests", (double)stats.https_requests);
    cJSON_AddNumberToObject(obj, "plaintext_blocked", (double)stats.plaintext_blocked);
    cJSON_AddNumberToObject(obj, "blocked_connections", (double)stats.blocked_connections);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    SVC_LOG_INFO("cupolas.net_get_stats: total=%llu active=%llu",
                 (unsigned long long)stats.total_connections,
                 (unsigned long long)stats.active_connections);
    return json;
}

int cupolas_service_entitlements_load(cupolas_service_t *svc,
                                      const cupolas_entitlements_load_params_t *params,
                                      cupolas_entitlements_load_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->yaml_path)
        return AIRY_ERR_INVALID_PARAM;

    cupolas_entitlements_t *loaded = NULL;
    int rc = cupolas_entitlements_load(params->yaml_path, &loaded);
    if (rc != 0 || !loaded) {
        SVC_LOG_ERROR("cupolas.entitlements_load failed: path=%s rc=%d", params->yaml_path, rc);
        out->loaded = 0;
        out->err = rc;
        return AIRY_ERR_UNKNOWN;
    }

    cupolas_entitlements_free(svc->entitlements);
    svc->entitlements = loaded;
    AIRY_FREE(svc->entitlements_path);
    svc->entitlements_path = AIRY_STRDUP(params->yaml_path);

    out->loaded = 1;
    out->err = 0;
    SVC_LOG_INFO("cupolas.entitlements_load: path=%s loaded", params->yaml_path);
    return AIRY_SUCCESS;
}

int cupolas_service_entitlements_check(cupolas_service_t *svc,
                                       const cupolas_entitlements_check_params_t *params,
                                       cupolas_entitlements_check_result_t *out)
{
    if (!svc || !params || !out)
        return AIRY_ERR_INVALID_PARAM;
    if (!params->kind || !params->param1)
        return AIRY_ERR_INVALID_PARAM;

    if (!svc->entitlements) {
        SVC_LOG_WARN("cupolas.entitlements_check: no entitlements loaded — call "
                     "cupolas.entitlements_load first (fail-closed)");
        out->allowed = 0;
        out->err = AIRY_ERR_STATE_ERROR;
        return AIRY_SUCCESS;
    }

    int allowed = 0;
    if (strcmp(params->kind, "fs") == 0) {
        allowed = cupolas_entitlements_check_fs(svc->entitlements, params->param1,
                                                params->param2 ? params->param2 : "read");
    } else if (strcmp(params->kind, "net") == 0) {
        uint16_t port = params->param2 ? (uint16_t)strtoul(params->param2, NULL, 10) : 0;
        allowed = cupolas_entitlements_check_net(svc->entitlements, params->param1, port, "tcp",
                                                 "outbound");
    } else if (strcmp(params->kind, "ipc") == 0) {
        allowed = cupolas_entitlements_check_ipc(svc->entitlements, params->param1,
                                                 params->param2 ? params->param2 : "call");
    } else if (strcmp(params->kind, "syscall") == 0) {
        allowed = cupolas_entitlements_check_syscall(svc->entitlements, params->param1);
    } else if (strcmp(params->kind, "capability") == 0) {
        allowed = cupolas_entitlements_check_capability(svc->entitlements, params->param1);
    } else if (strcmp(params->kind, "vault") == 0) {
        allowed = cupolas_entitlements_check_vault(svc->entitlements, params->param1,
                                                   params->param2 ? params->param2 : "read");
    } else {
        out->allowed = 0;
        out->err = AIRY_ERR_INVALID_PARAM;
        return AIRY_ERR_INVALID_PARAM;
    }

    out->allowed = (allowed > 0) ? 1 : 0;
    out->err = 0;
    SVC_LOG_INFO("cupolas.entitlements_check: kind=%s param=%s -> %s", params->kind, params->param1,
                 out->allowed ? "allowed" : "denied");
    return AIRY_SUCCESS;
}
