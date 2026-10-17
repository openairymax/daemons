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
#include "builtin/builtin_maths_internal.h"
#include "daemon_security.h"
#include "error.h"
#include "rpc/service.h"
#include "svc_logger.h"
#include "rpc/tool_service_internal.h"

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

/* Capability-grid row for the built-in tool set: only the strategy-side
 * fields that cannot be derived are written. id doubles as name and
 * permission_rule, executable is the derived "builtin:<id>" marker the
 * executor dispatches on, and BTOOL derives param_count from the params
 * array so the two can never drift. Adding a tool = one row here + its
 * implementation + one dispatch row in the grid in
 * src/builtin/builtin.c. */
typedef struct {
    char *id;
    char *description;
    tool_param_t *params;
    int param_count;
    int timeout_sec;
    int cacheable;
    tool_access_t access;
    const char *requires_sock; /* D8：依赖 daemon 的 socket 名（NULL=无外部依赖） */
} builtin_def_t;

#define BTOOL(id, desc, parr, tmo, cache, acc, dep)                                            \
    {id, desc, parr, (int)(sizeof(parr) / sizeof((parr)[0])), tmo, cache, acc, dep}

void register_builtin_tools(tool_service_t *svc)
{
    /* Params schema aligned with the OpenStandards tool description. The
     * required flags match the gateway tool schema's required array (SSoT,
     * T2 fix): fs_list.path is optional (builtin defaults to "." when
     * omitted), the rest are required. */
    static tool_param_t fs_path_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
    };
    static tool_param_t fs_write_params[] = {
        {"path", "{\"type\":\"string\"}", 1},
        {"content", "{\"type\":\"string\"}", 1},
        {"create_dirs", "{\"type\":\"boolean\"}", 0},
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
    static tool_param_t maths_plot_params[] = {
        {"expression", "{\"type\":\"string\"}", 1},
        {"xmin", "{\"type\":\"number\"}", 1},
        {"xmax", "{\"type\":\"number\"}", 1},
        {"samples", "{\"type\":\"integer\"}", 0},
    };

    /* Capability grid of the built-in tool set (SSoT): one row per tool.
     * Budget chain for the scanning tools is tool (20s) < runner IPC (30s)
     * < agent invoke; incident 0.1.16: fs_grep ran 158s and stalled the
     * DAG. */
    static const builtin_def_t defs[] = {
        BTOOL("fs_read", "Read a file's content from the local filesystem", fs_path_params, 30, 0,
              TOOL_ACCESS_READ, NULL),
        BTOOL("fs_write", "Write content to a file (create/overwrite)", fs_write_params, 30, 0,
              TOOL_ACCESS_WRITE, NULL),
        BTOOL("fs_list", "List entries of a local directory (JSON array)", fs_list_params, 20, 1,
              TOOL_ACCESS_READ, NULL),
        BTOOL("shell_run", "Execute a shell command and capture its output", shell_params, 60, 0,
              TOOL_ACCESS_WRITE, NULL),
        BTOOL("web_fetch", "Fetch a URL, return page body text", web_fetch_params, 45, 1,
              TOOL_ACCESS_READ, NULL),
        BTOOL("fs_glob", "List files matching a glob pattern (supports * ? and **)", glob_params,
              20, 0, TOOL_ACCESS_READ, NULL),
        BTOOL("fs_grep", "Search file contents with a regular expression (relpath:line:text)",
              grep_params, 20, 0, TOOL_ACCESS_READ, NULL),
        BTOOL("fs_edit", "Replace an exact string in a file (search-and-replace edit)", edit_params,
              30, 0, TOOL_ACCESS_WRITE, NULL),
        BTOOL("fs_delete",
              "Delete a local file, or a directory (recursive=1 for non-empty trees; destructive)",
              delete_params, 30, 0, TOOL_ACCESS_WRITE, NULL),
        BTOOL("web_search", "Search the web, return ranked results", web_search_params, 45, 1,
              TOOL_ACCESS_READ, NULL),
        BTOOL("git_exec",
              "Execute a read-only git command (whitelisted: "
              "status/diff/log/branch/show/ls-files/grep) and capture output",
              git_exec_params, 60, 0, TOOL_ACCESS_READ, NULL),
        BTOOL("git_diff", "Generate a unified diff for a path (git diff [--cached] [path])",
              git_diff_params, 60, 0, TOOL_ACCESS_READ, NULL),
        BTOOL("git_apply", "Apply a unified diff to the working tree", git_apply_params, 60, 0,
              TOOL_ACCESS_WRITE, NULL),
        BTOOL("maths_eval",
              "Evaluate a math expression precisely (arithmetic, powers, factorial, "
              "sqrt/sin/cos/tan/ln/log10/log2/exp/abs/min/max/floor/ceil etc.)",
              maths_eval_params, 10, 1, TOOL_ACCESS_READ, MATHS_D_SOCKET_NAME),
        BTOOL("maths_stats", "Compute numeric statistics (sum/mean/median/min/max/var/stddev)",
              maths_stats_params, 10, 1, TOOL_ACCESS_READ, MATHS_D_SOCKET_NAME),
        BTOOL("maths_plot",
              "Sample y=f(x) over [xmin,xmax] and return a ```plot block for "
              "TUI canvas rendering",
              maths_plot_params, 10, 1, TOOL_ACCESS_READ, MATHS_D_SOCKET_NAME),
    };

    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); ++i) {
        const builtin_def_t *d = &defs[i];
        char executable[48];
        snprintf(executable, sizeof(executable), "builtin:%s", d->id);
        tool_metadata_t meta = {
            .id = d->id,
            .name = d->id,
            .description = d->description,
            .executable = executable,
            .params = d->params,
            .param_count = (size_t)d->param_count,
            .timeout_sec = d->timeout_sec,
            .cacheable = d->cacheable,
            .access = d->access,
            .permission_rule = d->id,
        };
        int rc = tool_service_register(svc, &meta);
        if (rc == 0)
            SVC_LOG_INFO("Builtin tool registered: %s", d->id);
        else
            SVC_LOG_ERROR("Failed to register builtin tool: %s (rc=%d)", d->id, rc);

        daemon_security_add_acl_rule("tool_d", d->id, true);

        /* D8：把"工具 → 依赖 socket"的声明交给可用性视图，作为可达性联动
         * 的前提。声明失败（容量满/名过长）仅告警，工具仍注册。 */
        if (d->requires_sock && svc->avail &&
            tool_avail_dep(svc->avail, d->id, d->requires_sock) != 0)
            SVC_LOG_WARN("C-L06: TOOL-AVAIL: dep declare failed %s -> %s", d->id,
                         d->requires_sock);
    }
}