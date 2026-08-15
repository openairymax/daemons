// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
/**
 * @file service.c
 * @brief Tool service core logic.
 *
 * Improvements:
 * 1. Unified error codes to AIRY_ERR_*
 * 2. Completed streaming execution
 * 3. Thread safety
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

/* ---------- Built-in basic tool registration (fs_read / fs_write /
 * fs_list / shell_run) ----------
 */

/**
 * @brief Grant static tool ACL rules from the AIRY_AGENT_ACL env var.
 *
 * Fail-closed daemon_security denies every agent/tool pair without an ACL
 * entry; on servers without an interactive approver this would block all
 * agent tool use. This entry lets deployments pre-authorize built-in tools:
 *
 *   AIRY_AGENT_ACL="coding_v1=fs_read,fs_glob,shell_run;reviewer=fs_read"
 *
 * Format: ';'-separated agent rules, each "agent=tool1,tool2,...". Parsing
 * never fails the daemon: malformed segments are skipped with a warning.
 */
static void tool_service_register_acl_from_env(void)
{
    const char *rules = getenv("AIRY_AGENT_ACL");
    if (!rules || rules[0] == '\0')
        return;

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", rules);
    char *save1 = NULL;
    for (char *agent_rule = strtok_r(buf, ";", &save1); agent_rule;
         agent_rule = strtok_r(NULL, ";", &save1)) {
        char *eq = strchr(agent_rule, '=');
        if (!eq || eq == agent_rule) {
            SVC_LOG_WARN("AIRY_AGENT_ACL: malformed rule '%s' (expected agent=tool,...)",
                         agent_rule);
            continue;
        }
        *eq = '\0';
        char *save2 = NULL;
        for (char *tool = strtok_r(eq + 1, ",", &save2); tool;
             tool = strtok_r(NULL, ",", &save2)) {
            if (tool[0] == '\0')
                continue;
            int rc = daemon_security_add_acl_rule(agent_rule, tool, true);
            if (rc != 0)
                SVC_LOG_WARN("AIRY_AGENT_ACL: grant failed agent=%s tool=%s rc=%d", agent_rule,
                             tool, rc);
            else
                SVC_LOG_INFO("AIRY_AGENT_ACL: granted agent=%s tool=%s", agent_rule, tool);
        }
    }
}

/* ---------- Built-in tools registration (fs_read / fs_write / fs_list /
 * shell_run / web_fetch / fs_glob / fs_grep / fs_edit / web_search) ----------
 * Explicitly authorized via the daemon_security ACL (fail-closed:
 * unauthorized is always refused). executable uses "builtin:<id>" markers,
 * dispatched by the executor to the real implementations in builtin.c. */

static void register_builtin_tools(tool_service_t *svc)
{
    /* Static metadata (params schema aligned with the OpenStandards tool
     * description). The required flags match the gateway tool schema's
     * required array (SSoT, T2 fix): fs_list.path is optional (builtin
     * defaults to "." when omitted), the rest are required. */
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
    static tool_param_t git_exec_params[] = {
        {"command_args", "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}", 1},
        {"cwd", "{\"type\":\"string\"}", 0},
    };
    static tool_param_t git_diff_params[] = {
        {"path", "{\"type\":\"string\"}", 0},
        {"staged", "{\"type\":\"boolean\"}", 0},
    };
    static tool_param_t git_apply_params[] = {
        {"patch", "{\"type\":\"string\"}", 1},
        {"check_only", "{\"type\":\"boolean\"}", 0},
    };

    tool_metadata_t tools[12] = {
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
            .access = TOOL_ACCESS_READ,
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
            .access = TOOL_ACCESS_WRITE,
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
            .access = TOOL_ACCESS_READ,
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
            .access = TOOL_ACCESS_WRITE,
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
            .access = TOOL_ACCESS_READ,
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
            .access = TOOL_ACCESS_READ,
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
            .access = TOOL_ACCESS_READ,
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
            .access = TOOL_ACCESS_WRITE,
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
            .access = TOOL_ACCESS_READ,
        },
        {
            .id = "git_exec",
            .name = "git_exec",
            .description = "Execute a read-only git command (whitelisted: "
                           "status/diff/log/branch/show/ls-files/grep) and capture output",
            .executable = "builtin:git_exec",
            .params = git_exec_params,
            .param_count = 2,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "git_exec",
            .access = TOOL_ACCESS_READ,
        },
        {
            .id = "git_diff",
            .name = "git_diff",
            .description = "Generate a unified diff for a path (git diff [--cached] [path])",
            .executable = "builtin:git_diff",
            .params = git_diff_params,
            .param_count = 2,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "git_diff",
            .access = TOOL_ACCESS_READ,
        },
        {
            .id = "git_apply",
            .name = "git_apply",
            .description = "Apply a unified diff to the working tree (git apply [--check] -)",
            .executable = "builtin:git_apply",
            .params = git_apply_params,
            .param_count = 2,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "git_apply",
            .access = TOOL_ACCESS_WRITE,
        },
    };

    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i) {
        int rc = tool_service_register(svc, &tools[i]);
        if (rc == 0) {
            SVC_LOG_INFO("Builtin tool registered: %s", tools[i].id);
        } else {
            SVC_LOG_ERROR("Failed to register builtin tool: %s (rc=%d)", tools[i].id, rc);
        }

        daemon_security_add_acl_rule("tool_d", tools[i].id, true);
    }
}

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

/**
 * @brief Get tool metadata
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
 * @brief Validate tool params
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
 * @brief Get the cached tool result (cache key includes agent_id, avoiding
 *        cross-subject cache that would bypass approval)
 */
static tool_result_t *get_cached_result(tool_service_t *svc, tool_metadata_t *meta,
                                        const char *tool_id, const char *params_json,
                                        const char *agent_id)
{
    if (!svc || !meta || !tool_id || !params_json) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (!meta->cacheable) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *cache_key = tool_cache_key(tool_id, params_json, agent_id);
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
 * @brief Cache the tool result (cache key includes agent_id, consistent with
 *        the get path)
 */
static void cache_tool_result(tool_service_t *svc, tool_metadata_t *meta, const char *tool_id,
                              const char *params_json, const char *agent_id, tool_result_t *res)
{
    if (!svc || !meta || !tool_id || !params_json || !res || !res->success) {
        return;
    }

    if (!meta->cacheable) {
        return;
    }

    char *cache_key = tool_cache_key(tool_id, params_json, agent_id);
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
 * @brief Execute a tool
 */
static int do_execute_tool(tool_service_t *svc, tool_metadata_t *meta, const char *params_json,
                           const char *agent_id, tool_result_t **out_result)
{
    if (!svc || !meta || !out_result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    tool_result_t *res = NULL;
    int ret = tool_executor_run(svc->executor, meta, params_json, agent_id, &res);

    if (ret != 0) {
        SVC_LOG_ERROR("Tool execution failed, error: %d", ret);
        /* Keep result so the upper layer can read res->error (e.g. the
         * "User denied tool execution" of interactive approval deny/timeout);
         * released by the caller tool_service_execute. */
        *out_result = res;
        return ret;
    }

    *out_result = res;
    return AIRY_OK;
}

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

    tool_metadata_t *meta = get_tool_metadata(svc, req->tool_id);
    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    int valid = validate_tool_params(svc, meta, req->tool_id, req->params_json);
    if (valid <= 0) {
        tool_metadata_free(meta);
        return AIRY_ERROR_TOOL_VALIDATION;
    }

    /* 3. Check the cache.
     * P0 interactive-approval semantics: only subjects already authorized by
     * the static ACL may hit the cache. Unauthorized subjects (needing human
     * decision; allow is a one-time grant) must go through approval on every
     * call — even if previously approved and cached, this call's permission
     * confirmation cannot be skipped. */
    tool_result_t *cached_result = NULL;
    const char *subject = (req->agent_id && req->agent_id[0]) ? req->agent_id : "tool_d";
    if (daemon_check_tool_permission(subject, req->tool_id, "execute") == 0) {
        cached_result = get_cached_result(svc, meta, req->tool_id, req->params_json, req->agent_id);
    }
    if (cached_result) {
        tool_metadata_free(meta);
        *out_result = cached_result;
        svc->exec_total++;
        return AIRY_OK;
    }

    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = do_execute_tool(svc, meta, req->params_json, req->agent_id, &res);
    airy_time_monotonic(&ts1);
    svc->exec_total++;
    svc->exec_ms_total += airy_time_to_ms(&ts1) - airy_time_to_ms(&ts0);
    if (ret != 0) {
        svc->exec_fail++;
        tool_metadata_free(meta);
        meta = NULL;

        *out_result = res;
        return ret;
    }

    cache_tool_result(svc, meta, req->tool_id, req->params_json, req->agent_id, res);

    *out_result = res;
    if (meta) {
        tool_metadata_free(meta);
        meta = NULL;
    }
    return AIRY_OK;
}

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

    airy_mtx_lock(&svc->lock);
    tool_metadata_t *meta = tool_registry_get(svc->registry, req->tool_id);
    airy_mtx_unlock(&svc->lock);

    if (!meta) {
        SVC_LOG_ERROR("Tool not found: %s", req->tool_id);
        return AIRY_ERROR_TOOL_NOT_FOUND;
    }

    if (svc->validator) {
        int valid = tool_validator_validate(svc->validator, meta, req->params_json);
        if (!valid) {
            SVC_LOG_WARN("Parameter validation failed for tool: %s", req->tool_id);
            tool_metadata_free(meta);
            return AIRY_ERROR_TOOL_VALIDATION;
        }
    }

    if (!meta->executable || !strstr(meta->executable, "stream")) {

        SVC_LOG_INFO("Tool does not support streaming, using synchronous execution");
    }

    tool_result_t *res = NULL;
    airy_timestamp_t ts0, ts1;
    airy_time_monotonic(&ts0);
    int ret = tool_executor_run(svc->executor, meta, req->params_json, req->agent_id, &res);
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
