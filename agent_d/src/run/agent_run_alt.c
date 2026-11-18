/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_alt.c
 * @brief C-25 等价工具表的纯判定实现（换路执行在 run loop，不在此处）。
 *
 *        所有映射都拒绝「需要猜」的输入：命令按 ASCII 空白切分，任一 token
 *        出现引号、管道、重定向、通配或 $ 即整条放弃，宁可无替代也不构造
 *        一条语义改变的替代命令。
 */

#include "run/agent_run_alt.h"

#include "airy_memory.h"
#include "airy_types.h"
#include "error_codes.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
/* MSVC 无 strings.h，等价物是 string.h 里的 _stricmp。 */
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define ALT_CMD_MAX 512
#define ALT_TOK_MAX 24
#define ALT_TOK_LEN 128

/* 换路只接受可直接安全拼接的 token；`-` 开头允许（curl/wget 短旗标）。 */
static int alt_tok_ok(const char *s)
{
    size_t n = 0;
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; ++p) {
        char c = *p;
        if (++n > ALT_TOK_LEN)
            return 0;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            continue;
        if (strchr("._/@:+-=%", c))
            continue;
        return 0;
    }
    return 1;
}

static int alt_split(char *buf, char *tok[], int max)
{
    int n = 0;
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        if (n >= max)
            return -1;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        if (*p)
            *p++ = '\0';
    }
    for (int i = 0; i < n; ++i)
        if (!alt_tok_ok(tok[i]))
            return -1;
    return n;
}

static int alt_fill(run_alt_t *out, const char *tool, cJSON *args, const char *note)
{
    out->tool = AIRY_STRDUP(tool);
    out->args = cJSON_PrintUnformatted(args);
    out->note = AIRY_STRDUP(note);
    if (!out->tool || !out->args || !out->note) {
        run_alt_free(out);
        return 0;
    }
    return 1;
}

static int alt_cmd(run_alt_t *out, const char *cmd, const char *note)
{
    cJSON *a = cJSON_CreateObject();
    if (!a)
        return 0;
    cJSON_AddStringToObject(a, "command", cmd);
    int ok = alt_fill(out, "shell_run", a, note);
    cJSON_Delete(a);
    return ok;
}

/* shell_run(<特权程序> argv…) → priv_exec(program, argv)：同程序同参数，
 * 只换通道。表外程序与带路径的程序一律不换（授权仍由 priv_exec 白名单裁决，
 * 此处只是路由提示，两侧不一致时 priv_exec 会拒绝，失败方向安全）。 */
static int alt_priv(char *tok[], int n, run_alt_t *out)
{
    static const char *const prog[] = {"dnf", "rpm", "systemctl", NULL};
    int i = (strcmp(tok[0], "sudo") == 0) ? 1 : 0;
    if (i >= n)
        return 0;
    for (int k = 0; prog[k]; ++k) {
        if (strcmp(tok[i], prog[k]) != 0)
            continue;
        if (strchr(tok[i], '/') || i + 1 >= n)
            return 0;
        cJSON *a = cJSON_CreateObject();
        if (!a)
            return 0;
        cJSON_AddStringToObject(a, "program", tok[i]);
        cJSON *arr = cJSON_AddArrayToObject(a, "args");
        if (!arr) {
            cJSON_Delete(a);
            return 0;
        }
        for (int j = i + 1; j < n; ++j)
            cJSON_AddItemToArray(arr, cJSON_CreateString(tok[j]));
        int ok = alt_fill(out, "priv_exec", a,
                          "privileged program failed on the generic shell channel; "
                          "retry the same argv through the audited priv_exec channel");
        cJSON_Delete(a);
        return ok;
    }
    return 0;
}

/* 短旗标白名单：'-' 之后每个字符都须在 allowed 中（长旗标不在本表契约）。 */
static int alt_flags_ok(char *tok[], int n, const char *allowed)
{
    for (int i = 1; i < n; ++i) {
        if (tok[i][0] != '-')
            continue;
        for (const char *p = tok[i] + 1; *p; ++p)
            if (!strchr(allowed, *p))
                return 0;
    }
    return 1;
}

/* 单 URL 提取：只看位置参数，恰好一个 http(s) 才算数；旗标合法性由调用方
 * 先行判定。违背即返回 NULL（宁可无替代也不猜语义）。 */
static const char *alt_url_only(char *tok[], int n)
{
    const char *url = NULL;
    for (int i = 1; i < n; ++i) {
        if (tok[i][0] == '-')
            continue;
        if (url || strncmp(tok[i], "http", 4) != 0)
            return NULL;
        url = tok[i];
    }
    return url;
}

/* curl -s[S][L] URL → wget -qO- URL：只接受静默短旗标与单个 http(s) URL，
 * 其余旗标（-X/-d/-H/-o …）需要语义翻译，不做。 */
static int alt_curl2wget(char *tok[], int n, run_alt_t *out)
{
    if (!alt_flags_ok(tok, n, "sSL"))
        return 0;
    const char *url = alt_url_only(tok, n);
    if (!url)
        return 0;
    char cmd[ALT_CMD_MAX];
    snprintf(cmd, sizeof(cmd), "wget -qO- %s", url);
    return alt_cmd(out, cmd, "curl body fetch failed; retry the same URL with wget");
}

/* wget -qO- URL → curl -sSL URL：wget 静默正文取回的正向等价，跟随即 -L。 */
static int alt_wget2curl(char *tok[], int n, run_alt_t *out)
{
    for (int i = 1; i < n; ++i) {
        if (tok[i][0] != '-')
            continue;
        if (strcmp(tok[i], "-q") != 0 && strcmp(tok[i], "-O-") != 0 &&
            strcmp(tok[i], "-qO-") != 0)
            return 0;
    }
    const char *url = alt_url_only(tok, n);
    if (!url)
        return 0;
    char cmd[ALT_CMD_MAX];
    snprintf(cmd, sizeof(cmd), "curl -sSL %s", url);
    return alt_cmd(out, cmd, "wget body fetch failed; retry the same URL with curl");
}

static int alt_fetch(char *tok[], int n, run_alt_t *out)
{
    if (strcmp(tok[0], "curl") == 0)
        return alt_curl2wget(tok, n, out);
    if (strcmp(tok[0], "wget") == 0)
        return alt_wget2curl(tok, n, out);
    return 0;
}

void run_alt_free(run_alt_t *alt)
{
    if (!alt)
        return;
    AIRY_FREE(alt->tool);
    AIRY_FREE(alt->args);
    AIRY_FREE(alt->note);
    AIRY_MEMSET(alt, 0, sizeof(*alt));
}

int run_alt_enabled(void)
{
    const char *env = getenv("AIRY_TOOL_FALLBACK");
    if (!env || !*env)
        return 1;
    return !(strcasecmp(env, "off") == 0 || strcmp(env, "0") == 0 ||
             strcasecmp(env, "false") == 0 || strcasecmp(env, "no") == 0);
}

int run_alt_deny(int code)
{
    switch (code) {
    case AIRY_ERR_PERMISSION_DENIED:
    case AIRY_EPERM:
    case AIRY_EACCES:
    case AIRY_ERR_NOT_SUPPORTED:
    case AIRY_ENOTSUP:
        return 1;
    default:
        return 0;
    }
}

int run_alt_plan(const char *tool, const char *args, run_alt_t *out)
{
    if (!out)
        return 0;
    AIRY_MEMSET(out, 0, sizeof(*out));
    if (!run_alt_enabled() || !tool || !args || strcmp(tool, "shell_run") != 0)
        return 0;
    cJSON *root = cJSON_Parse(args);
    if (!root)
        return 0;
    int got = 0;
    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (cJSON_IsString(cmd) && cmd->valuestring && strlen(cmd->valuestring) < ALT_CMD_MAX) {
        char buf[ALT_CMD_MAX];
        char *tok[ALT_TOK_MAX];
        AIRY_STRNCPY_TERM(buf, cmd->valuestring, sizeof(buf));
        int n = alt_split(buf, tok, ALT_TOK_MAX);
        if (n > 0)
            got = alt_priv(tok, n, out) || alt_fetch(tok, n, out);
    }
    cJSON_Delete(root);
    return got;
}
