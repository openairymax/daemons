/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_audit.c
 * @brief D-9 审计留痕纯函数：风险分级 / 参数脱敏 / 输出摘要 / 沙箱标记。
 */

#include "job/tool_audit.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airy_memory.h"
#include "platform_sandbox.h"

/* 敏感键名（整键匹配，大小写不敏感）：命中即把值掩码，审计文件永不含明文。 */
static const char *const AUDIT_SECRET_KEYS[] = {
    "password", "passwd",  "passphrase",  "secret",       "token",
    "api_key",  "apikey",  "access_key",  "secret_key",   "private_key",
    "credential", "credentials", "authorization", "cookie", "session_id",
    "bearer",
};

/* 包管理/服务管理命令模式：即使不经 shell_run 也按高危处理。 */
static const char *const AUDIT_RISK_TOOLS[] = {"shell_run", "tool_create"};
static const char *const AUDIT_RISK_CMDS[] = {
    "apt-get",      "apt install",  "apt upgrade",  "yum install", "dnf install",
    "pacman -s",    "zypper ",      "apk add",      "brew install", "pip install",
    "pip3 install", "npm install",  "pnpm add",     "yarn add",    "gem install",
    "cargo install", "systemctl",   "service ",     "launchctl",   "chkconfig",
    "update-rc.d",  "rc-service",
};

static int audit_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int audit_find(const char *hay, const char *needle)
{
    size_t n = needle ? strlen(needle) : 0;

    if (!hay || n == 0)
        return 0;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i]) {
            char a = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] + 32) : p[i];
            char b = (needle[i] >= 'A' && needle[i] <= 'Z') ? (char)(needle[i] + 32) : needle[i];
            if (a != b)
                break;
            i++;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

static int audit_sensitive(const char *key)
{
    if (!key)
        return 0;
    for (size_t i = 0; i < sizeof(AUDIT_SECRET_KEYS) / sizeof(AUDIT_SECRET_KEYS[0]); i++) {
        if (audit_ieq(key, AUDIT_SECRET_KEYS[i]))
            return 1;
    }
    return 0;
}

/* 就地掩码字符串值：保留键与结构，不泄漏长度以外的任何信息。 */
static void audit_hide(char *s)
{
    size_t n = s ? strlen(s) : 0;

    if (n == 0)
        return;
    if (n >= 3) {
        s[0] = '*';
        s[1] = '*';
        s[2] = '*';
        s[3] = '\0';
        return;
    }
    AIRY_MEMSET(s, '*', n);
}

static void audit_mask_node(cJSON *node)
{
    cJSON *it = NULL;

    if (cJSON_IsObject(node)) {
        cJSON_ArrayForEach(it, node) {
            if (cJSON_IsString(it) && audit_sensitive(it->string))
                audit_hide(it->valuestring);
            else
                audit_mask_node(it);
        }
    } else if (cJSON_IsArray(node)) {
        cJSON_ArrayForEach(it, node)
            audit_mask_node(it);
    }
}

/* 单行化 + 定长截断：cap<4 时无法容纳省略号，直接给空串。 */
static void audit_line(const char *s, char *buf, size_t cap)
{
    size_t n = 0;
    size_t limit;

    if (!buf || cap == 0)
        return;
    if (!s || cap < 4) {
        buf[0] = '\0';
        return;
    }
    limit = cap - 4;
    while (*s && n < limit) {
        char c = *s++;
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        buf[n++] = c;
    }
    if (*s) {
        buf[n++] = '.';
        buf[n++] = '.';
        buf[n++] = '.';
    }
    buf[n] = '\0';
}

tool_audit_risk_t tool_audit_risk(const char *tool_id, const char *params_json, char *reason,
                                  size_t cap)
{
    if (reason && cap)
        reason[0] = '\0';

    for (size_t i = 0; i < sizeof(AUDIT_RISK_TOOLS) / sizeof(AUDIT_RISK_TOOLS[0]); i++) {
        if (tool_id && audit_ieq(tool_id, AUDIT_RISK_TOOLS[i])) {
            if (reason && cap)
                snprintf(reason, cap, "high-risk tool '%s'", tool_id);
            return TOOL_AUDIT_RISK_HIGH;
        }
    }
    for (size_t i = 0; i < sizeof(AUDIT_RISK_CMDS) / sizeof(AUDIT_RISK_CMDS[0]); i++) {
        if (audit_find(params_json, AUDIT_RISK_CMDS[i])) {
            if (reason && cap)
                snprintf(reason, cap, "package/service management command '%s'",
                         AUDIT_RISK_CMDS[i]);
            return TOOL_AUDIT_RISK_HIGH;
        }
    }
    return TOOL_AUDIT_RISK_LOW;
}

void tool_audit_mask(const char *params_json, char *buf, size_t cap)
{
    cJSON *root;
    char *text;

    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!params_json || !params_json[0])
        return;

    root = cJSON_Parse(params_json);
    if (!root) {
        /* 非法 JSON 仍要留痕：退化为原始串单行截断。 */
        audit_line(params_json, buf, cap);
        return;
    }
    audit_mask_node(root);
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
        return;
    audit_line(text, buf, cap);
    cJSON_free(text);
}

void tool_audit_sum(const char *text, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    if (!text || !text[0]) {
        buf[0] = '\0';
        return;
    }
    audit_line(text, buf, cap);
}

const char *tool_audit_sbox(void)
{
    const char *mode = getenv("AIRY_TOOL_SANDBOX_MODE");

    if (mode && audit_ieq(mode, "off"))
        return "off";
    if (airy_native_sandbox_landlock_available() > 0)
        return "landlock";
    return "degraded";
}
