// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file decl.c
 * @brief 期望态声明解析：画像 launch 段（profile.env）→ 进程表。
 *
 * profile.env 期望形态（launcher 写入，supervisor 只读）：
 *   AIRY_PROFILE=full
 *   AIRYRT_LAUNCH_CORE="gateway_d llm_d think_d agent_d tool_d"
 *   AIRYRT_LAUNCH_AUX="hook_d monit_d ... notify_d"
 *   AIRYRT_LAUNCH_ARGS_llm_d="--manager /path/model.yaml"
 *
 * 声明缺失时回落内置缺省表（仅升级兼容兜底；SSoT 仍是 launch 声明，
 * launcher 每次写画像时一并写入 launch 段，缺省表不构成第二套清单）。
 */

#include "supervisor_d.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef _WIN32
/* Windows 回环端口表 —— 与 gateway/src/biz/gateway_business_handler.c
 * WIN_SOCK_TCP 互为消费者副本，源头是各 daemon DEFAULT_TCP_PORT 实值。
 * maths_d 与 a2a_d 声明了相同端口 8087（既有冲突，B17 订正），故本表
 * 不含 maths；gateway_d 双平台均无 L2 端点，由 resolve_ep 统一置空
 * （进程存活腿）。 */
static const struct {
    const char *ns;
    const char *ep;
} SUP_WIN_TCP[] = {
    {"llm", "127.0.0.1:8080"},    {"tool", "127.0.0.1:8081"},
    {"market", "127.0.0.1:8082"}, {"sched", "127.0.0.1:8083"},
    {"notify", "127.0.0.1:8084"}, {"mem", "127.0.0.1:8085"},
    {"agent", "127.0.0.1:8086"},  {"a2a", "127.0.0.1:8087"},
    {"cupolas", "127.0.0.1:8089"},{"think", "127.0.0.1:8090"},
    {"hook", "127.0.0.1:8093"},   {"channel", "127.0.0.1:8094"},
    {"monit", "127.0.0.1:9090"},
};
#endif

static void path_join(char *out, size_t sz, const char *a, const char *b)
{
    snprintf(out, sz, "%s/%s", a, b);
}

static void home_default(char *out, size_t sz)
{
    const char *env = getenv("AIRY_HOME");
    if (env && *env) {
        snprintf(out, sz, "%s", env);
        return;
    }
#ifdef _WIN32
    const char *up = getenv("USERPROFILE");
    snprintf(out, sz, "%s\\.airymaxrt", up ? up : "C:");
#else
    const char *up = getenv("HOME");
    if (!up) {
        struct passwd *pw = getpwuid(getuid());
        up = pw ? pw->pw_dir : ".";
    }
    snprintf(out, sz, "%s/.airymaxrt", up);
#endif
}

/* name "notify_d" -> ns "notify"（去 _d 尾缀） */
static void ns_of(const char *name, char *out, size_t sz)
{
    size_t n = strlen(name);
    if (n > 2 && strcmp(name + n - 2, "_d") == 0)
        n -= 2;
    if (n >= sz)
        n = sz - 1;
    snprintf(out, sz, "%.*s", (int)n, name);
}

static void resolve_ep(sup_ctx_t *ctx, sup_proc_t *p)
{
    char ns[SUP_NAME_MAX];
    ns_of(p->name, ns, sizeof(ns));

    /* env 覆盖：AIRY_<NS>_SOCK（与 gateway 端点覆盖约定一致） */
    char env_key[64];
    snprintf(env_key, sizeof(env_key), "AIRY_%s_SOCK", ns);
    for (char *q = env_key; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    const char *ov = getenv(env_key);
    if (ov && *ov) {
        snprintf(p->sock, sizeof(p->sock), "%s", ov);
        return;
    }
    /* gateway_d 为 HTTP/WS 入口面，素无 L2 登记端点（launcher 诊断与
     * sock 等待历来跳过 gateway）：盲目合成 gateway.sock 会使假死探测
     * 永不通过而误杀，故置空走进程存活腿（同 Windows maths 模式）；
     * 运维经 AIRY_GATEWAY_SOCK 显式登记端点时仍生效。 */
    if (strcmp(ns, "gateway") == 0) {
        p->sock[0] = '\0';
        return;
    }
#ifndef _WIN32
    path_join(p->sock, sizeof(p->sock), ctx->runtime_dir, ns);
    strncat(p->sock, ".sock", sizeof(p->sock) - strlen(p->sock) - 1);
#else
    for (size_t i = 0; i < sizeof(SUP_WIN_TCP) / sizeof(SUP_WIN_TCP[0]); i++) {
        if (strcmp(ns, SUP_WIN_TCP[i].ns) == 0) {
            snprintf(p->sock, sizeof(p->sock), "%s", SUP_WIN_TCP[i].ep);
            return;
        }
    }
    /* 无登记端点（maths）：仅进程存活判定，sock 置空跳过假死探测 */
    p->sock[0] = '\0';
#endif
}

static void add_proc(sup_ctx_t *ctx, const char *name, sup_role_t role)
{
    if (ctx->count >= SUP_MAX_DAEMONS || !*name)
        return;
    if (sup_proc_find(ctx, name) >= 0)
        return; /* CORE/AUX 重复登记以先到为准 */
    sup_proc_t *p = &ctx->procs[ctx->count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->role = role;
    p->state = SUP_ST_STOPPED;
    p->pid = SUP_PID_INVALID;
    path_join(p->bin, sizeof(p->bin), ctx->airy_home, "bin");
    strncat(p->bin, "/", sizeof(p->bin) - strlen(p->bin) - 1);
    strncat(p->bin, name, sizeof(p->bin) - strlen(p->bin) - 1);
    resolve_ep(ctx, p);
}

static void split_names(sup_ctx_t *ctx, const char *val, sup_role_t role)
{
    char buf[SUP_ARGS_MAX];
    snprintf(buf, sizeof(buf), "%s", val);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        add_proc(ctx, tok, role);
}

/* 去首尾空白与成对引号 */
static char *trim_val(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

static long env_long(const char *key, long dflt)
{
    const char *v = getenv(key);
    if (!v || !*v)
        return dflt;
    char *end = NULL;
    long r = strtol(v, &end, 10);
    return (end && *end == '\0' && r > 0) ? r : dflt;
}

static void apply_args_line(sup_ctx_t *ctx, const char *key, const char *val)
{
    /* AIRYRT_LAUNCH_ARGS_<NAME> -> 声明注入 argv */
    static const char pfx[] = "AIRYRT_LAUNCH_ARGS_";
    size_t pl = sizeof(pfx) - 1;
    if (strncmp(key, pfx, pl) != 0)
        return;
    int i = sup_proc_find(ctx, key + pl);
    if (i < 0)
        return; /* 未登记的 args 声明拒绝（fail-closed） */
    snprintf(ctx->procs[i].args, sizeof(ctx->procs[i].args), "%s", val);
}

/* 内置缺省表：仅 profile.env 无 launch 段时兜底（升级兼容） */
int sup_decl_defaults(sup_ctx_t *ctx)
{
    ctx->count = 0;
    static const char *core[] = {"gateway_d", "llm_d", "think_d", "agent_d", "tool_d"};
    static const char *aux[] = {"hook_d",   "monit_d", "sched_d", "channel_d",
                                "market_d", "cupolas_d", "mem_d", "a2a_d", "notify_d"};
    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++)
        add_proc(ctx, core[i], SUP_ROLE_CORE);
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); i++)
        add_proc(ctx, aux[i], SUP_ROLE_AUX);
    return ctx->count > 0 ? 0 : -1;
}

int sup_decl_load(sup_ctx_t *ctx)
{
    home_default(ctx->airy_home, sizeof(ctx->airy_home));
    const char *rt = getenv("AIRY_RUNTIME_DIR");
    if (rt && *rt)
        snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s", rt);
    else
        path_join(ctx->runtime_dir, sizeof(ctx->runtime_dir), ctx->airy_home, "run");
    path_join(ctx->log_dir, sizeof(ctx->log_dir), ctx->airy_home, "logs");

    const char *ce = getenv("AIRY_SUPERVISOR_SOCK");
    if (ce && *ce)
        snprintf(ctx->ctrl_ep, sizeof(ctx->ctrl_ep), "%s", ce);
    else {
#ifndef _WIN32
        path_join(ctx->ctrl_ep, sizeof(ctx->ctrl_ep), ctx->runtime_dir, "supervisor.sock");
#else
        snprintf(ctx->ctrl_ep, sizeof(ctx->ctrl_ep), "127.0.0.1:8095");
#endif
    }

    ctx->tick_ms = env_long("AIRYRT_SUP_TICK_MS", SUP_DEFAULT_TICK_MS);
    ctx->backoff_base_ms = env_long("AIRYRT_SUP_BACKOFF_BASE_MS", SUP_DEFAULT_BACKOFF_BASE_MS);
    ctx->backoff_max_ms = env_long("AIRYRT_SUP_BACKOFF_MAX_MS", SUP_DEFAULT_BACKOFF_MAX_MS);
    ctx->max_attempts = (int)env_long("AIRYRT_SUP_MAX_ATTEMPTS", SUP_DEFAULT_MAX_ATTEMPTS);
    ctx->liveness_ticks = (int)env_long("AIRYRT_SUP_LIVENESS_TICKS", SUP_DEFAULT_LIVENESS_TICKS);
    ctx->shutdown = 0;

    char pfile[SUP_PATH_MAX];
    path_join(pfile, sizeof(pfile), ctx->airy_home, "config/profile.env");
    FILE *f = fopen(pfile, "r");
    if (!f)
        return sup_decl_defaults(ctx);

    ctx->count = 0;
    char line[SUP_ARGS_MAX + 64];
    int have_launch = 0;
    /* 第一遍：名单；第二遍：args（须在名单就位后应用） */
    for (int pass = 0; pass < 2; pass++) {
        rewind(f);
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq || eq == line)
                continue;
            *eq = '\0';
            char *key = line;
            char *val = trim_val(eq + 1);
            if (pass == 0) {
                if (strcmp(key, "AIRYRT_LAUNCH_CORE") == 0) {
                    have_launch = 1;
                    split_names(ctx, val, SUP_ROLE_CORE);
                } else if (strcmp(key, "AIRYRT_LAUNCH_AUX") == 0) {
                    have_launch = 1;
                    split_names(ctx, val, SUP_ROLE_AUX);
                }
            } else {
                apply_args_line(ctx, key, val);
            }
        }
    }
    fclose(f);
    if (!have_launch || ctx->count == 0)
        return sup_decl_defaults(ctx);
    return 0;
}

int sup_proc_find(const sup_ctx_t *ctx, const char *name)
{
    if (!name || !*name)
        return -1;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->procs[i].name, name) == 0)
            return i;
    }
    return -1;
}
