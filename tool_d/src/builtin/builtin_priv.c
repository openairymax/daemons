// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_priv.c
 * @brief 特权执行通道：白名单、通道开关、计划装配与出沙箱执行。
 *
 * shell_run 的每个子进程都套 Landlock + no_new_privs，提权结构上不可达。
 * 本域给出唯一显式通道，判定顺序固定为：通道开关 → 白名单 → 参数形状 →
 * euid → 审计 → 执行。任一环节不成立即拒绝，不留半开状态。
 */

#include "builtin/builtin_exec_internal.h"
#include "builtin/builtin_limits_internal.h"
#include "builtin/builtin_priv_internal.h"
#include "builtin/builtin_result_internal.h"
#include "daemon_security.h"

#ifndef _WIN32

#include <strings.h>
#include <unistd.h>

/* 程序白名单：模型传的是短名，这里给出唯一对应的绝对路径与参数族。
 * 扩展一个程序 = 本表加一行 + 对应族的校验器支持，不接受表外输入。 */
static const priv_prog_t k_priv_progs[] = {
    {"dnf", "/usr/bin/dnf", PRIV_FAMILY_PKG},
    {"rpm", "/usr/bin/rpm", PRIV_FAMILY_PKG},
    {"systemctl", "/usr/bin/systemctl", PRIV_FAMILY_SVC},
};

static int priv_is_off(const char *v)
{
    static const char *const off[] = {"off", "0", "false", "no", NULL};
    for (int i = 0; off[i]; ++i) {
        if (strcasecmp(v, off[i]) == 0)
            return 1;
    }
    return 0;
}

int priv_enabled(void)
{
    const char *v = getenv(PRIV_ENV_SWITCH);
    return (v && v[0]) ? !priv_is_off(v) : 1;
}

const priv_prog_t *priv_lookup(const char *name)
{
    if (!name || !name[0])
        return NULL;
    for (size_t i = 0; i < sizeof(k_priv_progs) / sizeof(k_priv_progs[0]); ++i) {
        if (strcmp(name, k_priv_progs[i].name) == 0)
            return &k_priv_progs[i];
    }
    return NULL;
}

/* 每程序一张动词/选项表：族只说明参数形状，真正的授权面按程序收窄，
 * 避免同族程序互相借道（如 dnf 不接受 rpm 的查询旗标）。 */
typedef struct {
    const char *name;
    const char *const *verbs;
    const char *const *opts;
} priv_policy_t;

static const char *const k_dnf_verbs[] = {"install", "remove", "reinstall", "upgrade",
                                          "update",  "clean",  "makecache", "list",
                                          "search",  "info",   "check-update", NULL};
static const char *const k_dnf_opts[] = {"-y",        "--assumeyes", "-q",       "--quiet",
                                         "--best",    "--refresh",   "--enablerepo",
                                         "--disablerepo", NULL};
static const char *const k_rpm_verbs[] = {"-q", "--query", "-V", "--verify", NULL};
static const char *const k_rpm_opts[] = {"--quiet", NULL};
static const char *const k_svc_verbs[] = {"status", "start",       "stop",     "restart",
                                          "enable", "disable",     "is-active", "is-enabled",
                                          "show",   "list-units",  "daemon-reload", NULL};
static const char *const k_svc_opts[] = {"--no-pager", "--now", "-q", "--quiet", NULL};

static const priv_policy_t k_priv_rules[] = {
    {"dnf", k_dnf_verbs, k_dnf_opts},
    {"rpm", k_rpm_verbs, k_rpm_opts},
    {"systemctl", k_svc_verbs, k_svc_opts},
};

static const priv_policy_t *priv_rule(const char *name)
{
    for (size_t i = 0; i < sizeof(k_priv_rules) / sizeof(k_priv_rules[0]); ++i) {
        if (strcmp(name, k_priv_rules[i].name) == 0)
            return &k_priv_rules[i];
    }
    return NULL;
}

static int priv_str_in(const char *s, const char *const *set)
{
    for (int i = 0; set[i]; ++i) {
        if (strcmp(s, set[i]) == 0)
            return 1;
    }
    return 0;
}

/* 参数只允许包名/单元名与白名单选项所需的字符集：不含空白、引号、
 * shell 元字符、路径分隔符与控制字符，故本地 rpm 路径安装一并被拒。 */
static int priv_arg_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) > PRIV_ARG_MAX_LEN)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9'))
            continue;
        switch (*p) {
        case '.': case '_': case '+': case '-': case ':': case '@': case '=':
            continue;
        default:
            return 0;
        }
    }
    return 1;
}

/* 首项必须是动词；其余非选项实参按字符集放行，选项必须在表内。 */
static int priv_args_ok(const priv_policy_t *rule, const cJSON *args)
{
    int seen_verb = 0;
    int n = cJSON_GetArraySize(args);
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_GetArrayItem(args, i);
        if (!cJSON_IsString(it) || !priv_arg_ok(it->valuestring))
            return -1;
        const char *a = it->valuestring;
        if (!seen_verb && priv_str_in(a, rule->verbs)) {
            seen_verb = 1;
            continue;
        }
        if (a[0] == '-') {
            if (!priv_str_in(a, rule->opts) && !priv_str_in(a, rule->verbs))
                return -1;
            continue;
        }
        if (!seen_verb)
            return -1;
    }
    return seen_verb ? 0 : -1;
}

int priv_plan_build(const char *params_json, priv_plan_t *plan, tool_result_t *res)
{
    if (!plan || !res)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(plan, 0, sizeof(*plan));

    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });

    const char *name = NULL;
    int rc = builtin_str_param(root, "program", &name, res);
    if (rc != AIRY_OK)
        return rc;
    if (strlen(name) > PRIV_PROG_MAX_LEN)
        return btool_fail(res, AIRY_ERR_EXEC_ARGS, "Program name too long");

    const priv_prog_t *prog = priv_lookup(name);
    const priv_policy_t *rule = prog ? priv_rule(name) : NULL;
    if (!prog || !rule)
        return btool_fail(res, AIRY_ERR_EXEC_NOT_FOUND,
                          "Program not in privileged whitelist: %s", name);

    cJSON *args = cJSON_GetObjectItem(root, "args");
    if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) == 0)
        return btool_fail(res, AIRY_ERR_EXEC_ARGS, "Missing non-empty args array");
    int argc = cJSON_GetArraySize(args);
    if (argc > PRIV_MAX_ARGS)
        return btool_fail(res, AIRY_ERR_EXEC_ARGS, "Too many arguments (max %d)", PRIV_MAX_ARGS);
    if (priv_args_ok(rule, args) != 0)
        return btool_fail(res, AIRY_ERR_EXEC_ARGS, "Argument rejected by %s policy", name);

    plan->program = prog;
    plan->argv[0] = (char *)prog->name;
    for (int i = 0; i < argc; ++i) {
        cJSON *it = cJSON_GetArrayItem(args, i);
        plan->argv[i + 1] = AIRY_STRDUP(it->valuestring);
        if (!plan->argv[i + 1]) {
            priv_plan_free(plan);
            return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Out of memory");
        }
        plan->argc = i + 1;
    }
    plan->argv[argc + 1] = NULL;
    return AIRY_OK;
}

void priv_plan_free(priv_plan_t *plan)
{
    if (!plan)
        return;
    for (int i = 1; i <= plan->argc; ++i) {
        AIRY_FREE(plan->argv[i]);
        plan->argv[i] = NULL;
    }
    plan->argc = 0;
    plan->program = NULL;
}

/* 审计资源串：program + 实参，截断到 448 字节内（审计行自身有上限），
 * 参数已通过字符集校验，不含空白以外需要转义的字符。 */
static void priv_audit(const priv_plan_t *plan, int rc)
{
    char res[512];
    size_t off = 0;
    res[0] = '\0';
    if (plan && plan->program) {
        int n = snprintf(res, sizeof(res) - 128, "%s", plan->program->path);
        off = (n > 0) ? (size_t)n : 0;
        for (int i = 1; i <= plan->argc && off < sizeof(res) - 128; ++i) {
            int m = snprintf(res + off, sizeof(res) - 128 - off, " %s", plan->argv[i]);
            if (m <= 0)
                break;
            off += (size_t)m;
        }
    } else {
        snprintf(res, sizeof(res), "(unparsed)");
    }
    daemon_audit_log_event("tool_d", "privileged_exec", res, rc, "tool_d");
}

/* 子进程侧：直接 execv 白名单路径，不经 shell、不套沙箱。 */
static void priv_spawn(void *ctx)
{
    const priv_plan_t *plan = (const priv_plan_t *)ctx;
    execv(plan->program->path, plan->argv);
    _exit(127);
}

int priv_exec_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
    if (!priv_enabled()) {
        priv_audit(NULL, -1);
        return btool_fail(res, AIRY_ERR_EXEC_PERMISSION,
                          "Privileged exec channel disabled by %s", PRIV_ENV_SWITCH);
    }

    priv_plan_t plan;
    int rc = priv_plan_build(params_json, &plan, res);
    if (rc != AIRY_OK) {
        priv_audit(&plan, -1);
        priv_plan_free(&plan);
        return rc;
    }

    if (geteuid() != 0) {
        priv_audit(&plan, -1);
        priv_plan_free(&plan);
        return btool_fail(res, AIRY_ERR_EXEC_PERMISSION,
                          "Privileged exec requires root (euid=%ld)",
                          (long)geteuid());
    }
    if (access(plan.program->path, X_OK) != 0) {
        priv_audit(&plan, -1);
        priv_plan_free(&plan);
        return btool_fail(res, AIRY_ERR_EXEC_NOT_FOUND, "Whitelisted program missing: %s",
                          plan.program->path);
    }

    char *out = NULL;
    int exit_code = -1;
    uint32_t tmo = timeout_ms ? timeout_ms : BUILTIN_SHELL_TIMEOUT_MS;
    int pr = builtin_proc_capture(priv_spawn, &plan, NULL, 0, tmo, &out, &exit_code, NULL);
    priv_audit(&plan, exit_code);
    priv_plan_free(&plan);
    if (pr != 0) {
        AIRY_FREE(out);
        return btool_fail(res, AIRY_ERR_EXEC_FAIL, "Failed to spawn privileged command");
    }
    return btool_finish(res, out, exit_code, "Privileged command");
}

#else /* _WIN32 */

int priv_enabled(void)
{
    return 0;
}

const priv_prog_t *priv_lookup(const char *name)
{
    (void)name;
    return NULL;
}

int priv_plan_build(const char *params_json, priv_plan_t *plan, tool_result_t *res)
{
    (void)params_json;
    if (plan)
        AIRY_MEMSET(plan, 0, sizeof(*plan));
    if (res)
        res->err_code = AIRY_ERR_EXEC_PERMISSION;
    return AIRY_ERR_EXEC_PERMISSION;
}

void priv_plan_free(priv_plan_t *plan)
{
    if (plan)
        AIRY_MEMSET(plan, 0, sizeof(*plan));
}

int priv_exec_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)params_json;
    (void)timeout_ms;
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
    return btool_fail(res, AIRY_ERR_EXEC_PERMISSION,
                      "Privileged exec channel is unavailable on this platform");
}

#endif /* _WIN32 */
