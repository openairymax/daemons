// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
/**
 * @file service.c
 * @brief 工具服务核心逻辑
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 改进说明：
 * 1. 统一错误码为 AIRY_ERR_*
 * 2. 完善流式执行功能
 * 3. 线程安全
 */

#include "daemon_defaults.h"
#include "daemon_security.h"
#include "error.h"
#include "executor.h"
#include "daemon_platform_ext.h"
#include "service.h"
#include "svc_logger.h"
#include "tool_approval.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>

/* ---------- 内置基础工具注册（fs_read / fs_write / fs_list / shell_run） ----------
 *
 * 通过 daemon_security ACL 显式授权（fail-closed：未授权一律拒绝）。
 * executable 使用 "builtin:<id>" 标记，由 executor 分派到 builtin.c 真实实现。 */

static void register_builtin_tools(tool_service_t *svc)
{
    /* 静态元数据（params schema 与 OpenStandards 工具描述对齐）
     * required 标志与 gateway 工具 schema required 数组一致（SSoT，T2 修复）：
     * fs_list.path 可选（builtin 实现省略时默认 "."），其余参数必需。 */
    static tool_param_t fs_path_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t fs_write_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
        {"content", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t shell_params[] = {
        {"command", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t fs_list_params[] = {
        {"path", "{\"type\":\"string\"}", 0},
    };
    static tool_param_t web_fetch_params[] = {
        {"url", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t glob_params[] = {
        {"pattern", "{\"type\":\"string\"}", 1},
        {"base", "{\"type\":\"string\"}", 0},
    };
    static tool_param_t grep_params[] = {
        {"pattern", "{\"type\":\"string\"}", 1},
        {"path", "{\"type\":\"string\"}", 0},
        {"glob", "{\"type\":\"string\"}", 0},
        {"max_results", "{\"type\":\"integer\"}", 0},
    };
    static tool_param_t edit_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
        {"old", "{\"type\":\"string\"}", 1},
        {"new", "{\"type\":\"string\"}", 1},
        {"count", "{\"type\":\"integer\"}", 0},
    };
    static tool_param_t web_search_params[] = {
        {"query", "{\"type\":\"string\"}", 1},
        {"max_results", "{\"type\":\"integer\"}", 0},
    };

    tool_metadata_t tools[9] = {
        {
            .id = "fs_read",
            .name = "fs_read",
            .description = "Read a file's content from the local filesystem",
            .executable = "builtin:fs_read",
            .params = fs_path_params,
            .param_count = 1,
            .timeout_sec = 30,
            .cacheable = 0,
            .permission_rule = "fs_read",
        },
        {
            .id = "fs_write",
            .name = "fs_write",
            .description = "Write content to a local file (creates or overwrites)",
            .executable = "builtin:fs_write",
            .params = fs_write_params,
            .param_count = 2,
            .timeout_sec = 30,
            .cacheable = 0,
            .permission_rule = "fs_write",
        },
        {
            .id = "fs_list",
            .name = "fs_list",
            .description = "List entries of a local directory (JSON array)",
            .executable = "builtin:fs_list",
            .params = fs_list_params,
            .param_count = 1,
            .timeout_sec = 30,
            .cacheable = 1,
            .permission_rule = "fs_list",
        },
        {
            .id = "shell_run",
            .name = "shell_run",
            .description = "Execute a shell command and capture its output",
            .executable = "builtin:shell_run",
            .params = shell_params,
            .param_count = 1,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "shell_run",
        },
        {
            .id = "web_fetch",
            .name = "web_fetch",
            .description = "Fetch a web page over HTTP(S) and return its body text",
            .executable = "builtin:web_fetch",
            .params = web_fetch_params,
            .param_count = 1,
            .timeout_sec = 45,
            .cacheable = 1,
            .permission_rule = "web_fetch",
        },
        {
            .id = "fs_glob",
            .name = "fs_glob",
            .description = "List files matching a glob pattern (supports * ? and **)",
            .executable = "builtin:fs_glob",
            .params = glob_params,
            .param_count = 2,
            .timeout_sec = 30,
            .cacheable = 0,
            .permission_rule = "fs_glob",
        },
        {
            .id = "fs_grep",
            .name = "fs_grep",
            .description = "Search file contents with a regular expression (relpath:line:text)",
            .executable = "builtin:fs_grep",
            .params = grep_params,
            .param_count = 4,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "fs_grep",
        },
        {
            .id = "fs_edit",
            .name = "fs_edit",
            .description = "Replace an exact string in a file (search-and-replace edit)",
            .executable = "builtin:fs_edit",
            .params = edit_params,
            .param_count = 4,
            .timeout_sec = 30,
            .cacheable = 0,
            .permission_rule = "fs_edit",
        },
        {
            .id = "web_search",
            .name = "web_search",
            .description = "Search the web (DuckDuckGo) and return ranked results",
            .executable = "builtin:web_search",
            .params = web_search_params,
            .param_count = 2,
            .timeout_sec = 45,
            .cacheable = 1,
            .permission_rule = "web_search",
        },
    };

    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i) {
        int rc = tool_service_register(svc, &tools[i]);
        if (rc == 0) {
            SVC_LOG_INFO("Builtin tool registered: %s", tools[i].id);
        } else {
            SVC_LOG_ERROR("Failed to register builtin tool: %s (rc=%d)", tools[i].id, rc);
        }
        /* ACL 授权：agent tool_d 可执行该工具（fail-closed 表） */
        daemon_security_add_acl_rule("tool_d", tools[i].id, true);
    }
}

/* ---------- 工具服务创建 ---------- */

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

    /* 创建注册表 */
    svc->registry = tool_registry_create(NULL);
    if (!svc->registry) {
        SVC_LOG_ERROR("Failed to create registry");
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* 创建执行器 */
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

    /* P3.17 (ACC-DT18): 默认启用工具审批（enable_approval=true）。
     * 创建 approval_ctx 并注入 executor，使所有工具执行必须通过 Cupolas 安全穹顶审批。
     * executor.c 已改为 fail-closed：未注入 approval_ctx 时拒绝执行。
     * daemon_security 采用 fail-closed ACL：无 ACL 条目 = 拒绝。
     * 部署时需通过 daemon_security_add_acl_rule() 注册授权的工具。*/
    daemon_security_init(NULL, NULL);

    /* 注册内置基础工具（fs_read/fs_write/fs_list/shell_run）+ ACL 授权 */
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

    /* 创建验证器 */
    svc->validator = tool_validator_create();
    if (!svc->validator) {
        SVC_LOG_ERROR("Failed to create validator");
        tool_executor_destroy(svc->executor);
        tool_registry_destroy(svc->registry);
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    /* 创建缓存（默认配置） */
    svc->cache = tool_cache_create(1024, 3600);
    if (!svc->cache) {
        SVC_LOG_WARN("Cache creation failed, continuing without cache");
    }

    SVC_LOG_INFO("Tool service initialized successfully");
    return svc;
}

/* ---------- 工具服务销毁 ---------- */

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

/* ---------- 工具注册 ---------- */

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

/* ---------- 辅助函数（降低 tool_service_execute 复杂度） ---------- */

/**
 * @brief 获取工具元数据
 */
static tool_metadata_t *get_tool_metadata(tool_service_t *svc, const char *tool_id)
{
    if (!svc || !tool_id) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, tool_id);
    airy_mtx_unlock(&svc->lock);

    return meta;
}

/**
 * @brief 验证工具参数
 */
static int validate_tool_params(tool_service_t *svc, tool_metadata_t *meta, const char *tool_id,
                                const char *params_json)
{
    if (!svc || !meta || !tool_id) {
        return AIRY_EINVAL;
    }

    if (svc->validator) {
        int valid = tool_validator_validate(svc->validator, meta, params_json);
        if (!valid) {
            SVC_LOG_WARN("Parameter validation failed for tool: %s", tool_id);
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 获取缓存的工具结果
 */
static tool_result_t *get_cached_result(tool_service_t *svc, tool_metadata_t *meta,
                                        const char *tool_id, const char *params_json)
{
    if (!svc || !meta || !tool_id || !params_json) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (!meta->cacheable) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *cache_key = tool_cache_key(tool_id, params_json);
    if (!cache_key) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *cached = NULL;
    if (tool_cache_get(svc->cache, cache_key, &cached) == 1 && cached) {
        tool_result_t *res = tool_result_from_json(cached);
        AIRY_FREE(cached);
        cached = NULL;
        if (res) {
            SVC_LOG_DEBUG("Cache hit for tool: %s", tool_id);
            AIRY_FREE(cache_key);
            return res;
        }
        SVC_LOG_WARN("Failed to parse cached result for tool: %s", tool_id);
    }

    AIRY_FREE(cache_key);
    cache_key = NULL;
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/**
 * @brief 缓存工具结果
 */
static void cache_tool_result(tool_service_t *svc, tool_metadata_t *meta, const char *tool_id,
                              const char *params_json, tool_result_t *res)
{
    if (!svc || !meta || !tool_id || !params_json || !res || !res->success) {
        return;
    }

    if (!meta->cacheable) {
        return;
    }

    char *cache_key = tool_cache_key(tool_id, params_json);
    if (!cache_key) {
        return;
    }

    char *res_json = tool_result_to_json(res);
    if (res_json) {
        tool_cache_put(svc->cache, cache_key, res_json);
        AIRY_FREE(res_json);
        res_json = NULL;
    }

    AIRY_FREE(cache_key);
    cache_key = NULL;
}

/**
 * @brief 执行工具
 */
static int do_execute_tool(tool_service_t *svc, tool_metadata_t *meta, const char *params_json,
                           tool_result_t **out_result)
{
    if (!svc || !meta || !out_result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    tool_result_t *res = NULL;
    int ret = tool_executor_run(svc->executor, meta, params_json, &res);

    if (ret != 0) {
        SVC_LOG_ERROR("Tool execution failed, error: %d", ret);
        if (res) {
            tool_result_free(res);
            res = NULL;
        }
        return ret;
    }

    *out_result = res;
    return AIRY_OK;
}

/* ---------- 工具执行（重构后：圈复杂度从 18 降至 8） ---------- */

int tool_service_execute(tool_service_t *svc, const tool_execute_request_t *req,
                         tool_result_t **out_result)
{
    if (!svc || !req || !out_result) {
        SVC_LOG_ERROR("Invalid parameters to tool_service_execute");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!req->tool_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    /* 1. 获取工具元数据 */
    tool_metadata_t *meta = get_tool_metadata(svc, req->tool_id);
    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    /* 2. 验证参数 */
    int valid = validate_tool_params(svc, meta, req->tool_id, req->params_json);
    if (valid <= 0) {
        tool_metadata_free(meta);
        return AIRY_ERROR_TOOL_VALIDATION;
    }

    /* 3. 检查缓存 */
    tool_result_t *cached_result = get_cached_result(svc, meta, req->tool_id, req->params_json);
    if (cached_result) {
        tool_metadata_free(meta);
        *out_result = cached_result;
        svc->exec_total++; /* 缓存命中同样计入执行统计 */
        return AIRY_OK;
    }

    /* 4. 执行工具 */
    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = do_execute_tool(svc, meta, req->params_json, &res);
    airy_time_monotonic(&ts1);
    svc->exec_total++;
    svc->exec_ms_total += airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0);
    if (ret != 0) {
        svc->exec_fail++;
        tool_metadata_free(meta);
        meta = NULL;
        return ret;
    }

    /* 5. 存入缓存 */
    cache_tool_result(svc, meta, req->tool_id, req->params_json, res);

    *out_result = res;
    if (meta) {
        tool_metadata_free(meta);
        meta = NULL;
    }
    return AIRY_OK;
}

/* ---------- 流式执行 ---------- */

int tool_service_execute_stream(tool_service_t *svc, const tool_execute_request_t *req,
                                tool_stream_callback_t callback, void *callback_data,
                                tool_result_t **out_result)
{
    if (!svc || !req || !callback) {
        SVC_LOG_ERROR("Invalid parameters to tool_service_execute_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!req->tool_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    /* 1. 获取工具元数据 */
    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, req->tool_id);
    airy_mtx_unlock(&svc->lock);

    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    /* 2. 验证参数 */
    if (svc->validator) {
        int valid = tool_validator_validate(svc->validator, meta, req->params_json);
        if (!valid) {
            SVC_LOG_WARN("Parameter validation failed for tool: %s", req->tool_id);
            tool_metadata_free(meta);
            return AIRY_ERROR_TOOL_VALIDATION;
        }
    }

    /* 3. 检查是否支持流式 */
    if (!meta->executable || !strstr(meta->executable, "stream")) {
        /* 工具不支持流式，使用普通执行并逐块返回 */
        SVC_LOG_INFO("Tool does not support streaming, using synchronous execution");
    }

    /* 4. 执行工具（带流式回调） */
    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = tool_executor_run(svc->executor, meta, req->params_json, &res);
    airy_time_monotonic(&ts1);
    svc->exec_total++;
    svc->exec_ms_total += airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0);

    if (ret == 0 && res) {
        if (callback) {
            if (res->output) {
                callback(res->output, 0, callback_data);
            }
            if (res->error) {
                callback(res->error, 1, callback_data);
            }
        }
        if (out_result) {
            *out_result = res;
        }
    } else {
        if (out_result) {
            *out_result = res;
        }
    }

    if (ret != 0) {
        svc->exec_fail++;
        SVC_LOG_ERROR("Tool stream execution failed: %s, error: %d", req->tool_id, ret);
    }

    tool_metadata_free(meta);
    return ret;
}

/* ---------- 工具结果释放 ---------- */

char *tool_service_get_stats(tool_service_t *svc)
{
    if (!svc)
        return NULL;

    /* 注册工具数：复用 tool_service_list（真实 registry 内容） */
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
                            svc->exec_total ? (double)svc->exec_ms_total / (double)svc->exec_total
                                            : 0.0);
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

/* ---------- 工具元数据释放 ---------- */

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
