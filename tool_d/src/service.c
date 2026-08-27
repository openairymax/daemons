// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file service.c
 * @brief Tool service 生命周期/注册表/统计/资源释放/交互审批域。
 *
 * 2026-08-27 域拆分（原 888 行 → 3 文件）：builtin 工具注册与
 * AIRY_AGENT_ACL 预授权见 service_builtin.c，工具执行（同步/流式 + 缓存）
 * 见 service_execute.c；跨文件注册函数经 tool_service_internal.h 声明。
 */

#include "daemon_defaults.h"
#include "daemon_security.h"
#include "error.h"
#include "executor.h"
#include "daemon_platform_ext.h"
#include "service.h"
#include "svc_logger.h"
#include "tool_approval.h"
#include "tool_service_internal.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>

tool_service_t *tool_service_create(const char *config_path __attribute__((unused)))
{

    tool_service_t *svc = (tool_service_t *)AIRY_CALLOC(1, sizeof(tool_service_t));
    if (!svc) {
        SVC_LOG_ERROR("Failed to allocate tool service");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (airy_mtx_init(&svc->lock) != 0) {
        SVC_LOG_ERROR("Failed to initialize service lock");
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    svc->registry = tool_registry_create(NULL);
    if (!svc->registry) {
        SVC_LOG_ERROR("Failed to create registry");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    tool_executor_config_t exec_config;
    __builtin_memset(&exec_config, 0, sizeof(exec_config));
    exec_config.timeout_sec = AIRY_DEFAULT_TIMEOUT_SEC;

    svc->executor = tool_executor_create_ex(&exec_config);
    if (!svc->executor) {
        SVC_LOG_ERROR("Failed to create executor");
        tool_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* P3.17 (ACC-DT18): enable tool approval by default
     * (enable_approval=true). Create approval_ctx and inject it into the
     * executor so every tool execution must pass Cupolas safety-dome
     * approval. executor.c is fail-closed: without an injected approval_ctx
     * it refuses execution. daemon_security uses a fail-closed ACL: no ACL
     * entry = denied. Deployment must register authorized tools via
     * daemon_security_add_acl_rule(). */
    {
        /* 工具授权 ACL 的权威源：$AIRY_CONFIG_DIR/permission_rules.yaml
         * （默认随 AIRY_HOME 初始化落地）。此前仅依赖 AIRY_AGENT_ACL 环境
         * 变量，daemon 由 /daemon start 拉起时环境变量易丢失，导致 ACL 空、
         * 所有 agent 工具调用被 fail-closed 拒绝（agent 任务无法执行）。
         * 文件不存在时仅告警（保持 fail-closed 语义），不阻断启动。 */
        static daemon_security_config_t sec_cfg;
        __builtin_memset(&sec_cfg, 0, sizeof(sec_cfg));
        static char rules_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir && cfg_dir[0]) {
            int plen = snprintf(rules_path, sizeof(rules_path), "%s/permission_rules.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(rules_path))
                sec_cfg.permission_rules_path = rules_path;
        }
        daemon_security_init(&sec_cfg, NULL);
    }

    /* Static ACL pre-authorization from AIRY_AGENT_ACL (server deployments
     * without an interactive approver). Must run after daemon_security_init. */
    tool_service_register_acl_from_env();

    register_builtin_tools(svc);

    tool_approval_config_t approval_cfg;
    __builtin_memset(&approval_cfg, 0, sizeof(approval_cfg));
    approval_cfg.agent_id = "tool_d";
    approval_cfg.enable_safety_guard_chain = true;
    approval_cfg.enable_audit_logging = true;
    approval_cfg.permission_rules = NULL;
    tool_approval_ctx_t *approval_ctx = tool_approval_create(&approval_cfg);
    if (approval_ctx) {
        tool_executor_set_approval_ctx(svc->executor, approval_ctx);
        SVC_LOG_INFO("C-L05: Default tool approval context attached (enable_approval=true)");
    } else {
        SVC_LOG_ERROR("C-L05: Failed to create default approval context — "
                      "executor will fail-closed on all tool executions");
    }

    svc->validator = tool_validator_create();
    if (!svc->validator) {
        SVC_LOG_ERROR("Failed to create validator");
        tool_executor_destroy(svc->executor);
        tool_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    svc->cache = tool_cache_create(1024, 3600);
    if (!svc->cache) {
        SVC_LOG_WARN("Cache creation failed, continuing without cache");
    }

    SVC_LOG_INFO("Tool service initialized successfully");
    return svc;
}

void tool_service_destroy(tool_service_t *svc)
{
    if (!svc)
        return;

    if (svc->registry) {
        tool_registry_destroy(svc->registry);
        svc->registry = NULL;
    }

    if (svc->executor) {
        tool_executor_destroy(svc->executor);
        svc->executor = NULL;
    }

    if (svc->validator) {
        tool_validator_destroy(svc->validator);
        svc->validator = NULL;
    }

    if (svc->cache) {
        tool_cache_destroy(svc->cache);
        svc->cache = NULL;
    }

    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int tool_service_register(tool_service_t *svc, const tool_metadata_t *meta)
{
    if (!svc || !meta || !meta->id) {
        SVC_LOG_ERROR("Invalid parameters to tool_service_register");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&svc->lock);
    int ret = tool_registry_add(svc->registry, meta);
    airy_mtx_unlock(&svc->lock);

    if (ret == 0) {
        SVC_LOG_INFO("Registered tool: %s", meta->id);
    } else {
        SVC_LOG_ERROR("Failed to register tool: %s", meta->id);
    }

    return ret;
}

int tool_service_unregister(tool_service_t *svc, const char *tool_id)
{
    if (!svc || !tool_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&svc->lock);
    int ret = tool_registry_remove(svc->registry, tool_id);
    airy_mtx_unlock(&svc->lock);

    if (ret == 0) {
        SVC_LOG_INFO("Unregistered tool: %s", tool_id);
    }

    return ret;
}

tool_metadata_t *tool_service_get(tool_service_t *svc, const char *tool_id)
{
    if (!svc || !tool_id) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, tool_id);
    airy_mtx_unlock(&svc->lock);

    return meta;
}

char *tool_service_list(tool_service_t *svc)
{
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    char *json = tool_registry_list_json(svc->registry);
    airy_mtx_unlock(&svc->lock);

    return json;
}

char *tool_service_get_stats(tool_service_t *svc)
{
    if (!svc)
        return NULL;

    char *list = tool_service_list(svc);
    cJSON *arr = list ? cJSON_Parse(list) : NULL;
    int tool_count = arr ? cJSON_GetArraySize(arr) : 0;
    if (arr)
        cJSON_Delete(arr);
    AIRY_FREE(list);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddStringToObject(root, "daemon", "tool_d");
    cJSON_AddNumberToObject(root, "tools", tool_count);
    cJSON_AddNumberToObject(root, "exec_total", (double)svc->exec_total);
    cJSON_AddNumberToObject(root, "exec_fail", (double)svc->exec_fail);
    cJSON_AddNumberToObject(root, "exec_ms_total", (double)svc->exec_ms_total);
    cJSON_AddNumberToObject(root, "avg_exec_ms",
                            svc->exec_total ? (double)svc->exec_ms_total / (double)svc->exec_total :
                                              0.0);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void tool_result_free(tool_result_t *res)
{
    if (!res)
        return;
    AIRY_FREE(res->output);
    AIRY_FREE(res->error);
    AIRY_FREE(res);
}

void tool_metadata_free(tool_metadata_t *meta)
{
    if (!meta)
        return;
    AIRY_FREE(meta->id);
    AIRY_FREE(meta->name);
    AIRY_FREE(meta->description);
    AIRY_FREE(meta->executable);

    for (size_t i = 0; i < meta->param_count; ++i) {
        AIRY_FREE((void *)meta->params[i].name);
        AIRY_FREE((void *)meta->params[i].schema);
    }
    AIRY_FREE(meta->params);

    AIRY_FREE(meta->permission_rule);
    AIRY_FREE(meta);
}

char *tool_service_interactive_pending_list(tool_service_t *svc)
{
    if (!svc || !svc->executor) {
        return NULL;
    }
    return tool_executor_interactive_pending_list(svc->executor);
}

int tool_service_interactive_resolve(tool_service_t *svc, const char *request_id,
                                     const char *decision)
{
    if (!svc || !request_id || !decision) {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!svc->executor) {
        return AIRY_ERR_NOT_FOUND;
    }
    return tool_executor_interactive_resolve(svc->executor, request_id, decision);
}
