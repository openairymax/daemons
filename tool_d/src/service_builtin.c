// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_builtin.c
 * @brief Tool service builtin 工具注册域：AIRY_AGENT_ACL 静态预授权与
 *        builtin 工具（fs_* / shell_run / web_* / git_* / maths_*）元数据
 *        注册（fail-closed 语义：未授权一律拒绝）。
 *
 * 2026-08-27 域拆分（原 service.c 888 行 → 3 文件）：生命周期/注册表/统计
 * 域见 service.c，工具执行域见 service_execute.c。
 */

#include "airy_memory.h"
#include "daemon_security.h"
#include "error.h"
#include "executor.h"
#include "service.h"
#include "svc_logger.h"
#include "tool_service_internal.h"

#include <stdio.h>
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
void tool_service_register_acl_from_env(void)
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

void register_builtin_tools(tool_service_t *svc)
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
        {"cwd", "{\"type\":\"string\"}", 0},
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
    static tool_param_t delete_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
        {"recursive", "{\"type\":\"boolean\"}", 0},
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
    static tool_param_t maths_eval_params[] = {
        {"expression", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t maths_stats_params[] = {
        {"op", "{\"type\":\"string\"}", 1},
        {"values", "{\"type\":\"array\",\"items\":{\"type\":\"number\"}}", 1},
    };

    tool_metadata_t tools[15] = {
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
            .description = "Write content to a file (create/overwrite)",
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
            .description = "Fetch a URL, return page body text",
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
            .id = "fs_delete",
            .name = "fs_delete",
            .description = "Delete a local file, or a directory (recursive=1 "
                           "for non-empty trees; destructive)",
            .executable = "builtin:fs_delete",
            .params = delete_params,
            .param_count = 2,
            .timeout_sec = 30,
            .cacheable = 0,
            .permission_rule = "fs_delete",
            .access = TOOL_ACCESS_WRITE,
        },
        {
            .id = "web_search",
            .name = "web_search",
            .description = "Search the web, return ranked results",
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
            .description = "Apply a unified diff to the working tree",
            .executable = "builtin:git_apply",
            .params = git_apply_params,
            .param_count = 2,
            .timeout_sec = 60,
            .cacheable = 0,
            .permission_rule = "git_apply",
            .access = TOOL_ACCESS_WRITE,
        },
        {
            .id = "maths_eval",
            .name = "maths_eval",
            .description = "Evaluate a math expression precisely (arithmetic, "
                           "powers, factorial, sqrt/sin/cos/tan/ln/log10/log2/"
                           "exp/abs/min/max/floor/ceil etc.)",
            .executable = "builtin:maths_eval",
            .params = maths_eval_params,
            .param_count = 1,
            .timeout_sec = 10,
            .cacheable = 1,
            .permission_rule = "maths_eval",
            .access = TOOL_ACCESS_READ,
        },
        {
            .id = "maths_stats",
            .name = "maths_stats",
            .description = "Compute numeric statistics (sum/mean/median/min/max/var/stddev)",
            .executable = "builtin:maths_stats",
            .params = maths_stats_params,
            .param_count = 2,
            .timeout_sec = 10,
            .cacheable = 1,
            .permission_rule = "maths_stats",
            .access = TOOL_ACCESS_READ,
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
