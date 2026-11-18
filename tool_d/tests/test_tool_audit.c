// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_tool_audit.c
 * @brief D-9 审计留痕验收：风险分级 / 参数脱敏 / 输出摘要 / 沙箱标记。
 */

#include "job/tool_audit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_risk(void)
{
    char reason[64];

    assert(tool_audit_risk("shell_run", "{\"command\":\"echo hi\"}", reason,
                           sizeof(reason)) == TOOL_AUDIT_RISK_HIGH);
    assert(strstr(reason, "high-risk") != NULL);

    assert(tool_audit_risk("tool_create", "{\"name\":\"x\"}", reason, sizeof(reason)) ==
           TOOL_AUDIT_RISK_HIGH);

    assert(tool_audit_risk("fs_read", "{\"path\":\"/tmp/a\"}", reason, sizeof(reason)) ==
           TOOL_AUDIT_RISK_LOW);
    assert(reason[0] == '\0');

    /* 非 shell_run 工具但命中包管理/服务管理命令模式，同样判高危。 */
    assert(tool_audit_risk("run_cmd", "{\"command\":\"systemctl restart nginx\"}", reason,
                           sizeof(reason)) == TOOL_AUDIT_RISK_HIGH);
    assert(strstr(reason, "systemctl") != NULL);

    assert(tool_audit_risk("run_cmd", "{\"command\":\"apt-get install -y curl\"}", reason,
                           sizeof(reason)) == TOOL_AUDIT_RISK_HIGH);
    assert(tool_audit_risk(NULL, NULL, NULL, 0) == TOOL_AUDIT_RISK_LOW);
}

static void test_mask(void)
{
    char buf[512];

    tool_audit_mask("{\"path\":\"/tmp/a\",\"password\":\"s3cr3t\","
                    "\"nested\":{\"api_key\":\"sk-12345\"},\"token\":\"abc\"}",
                    buf, sizeof(buf));
    assert(strstr(buf, "s3cr3t") == NULL);
    assert(strstr(buf, "sk-12345") == NULL);
    assert(strstr(buf, "\"abc\"") == NULL);
    assert(strstr(buf, "***") != NULL);
    /* 非敏感值保留，审计仍可还原调用参数。 */
    assert(strstr(buf, "/tmp/a") != NULL);

    /* 非法 JSON：退化为单行原始串，不因解析失败丢审计。 */
    tool_audit_mask("line1\nline2", buf, sizeof(buf));
    assert(strchr(buf, '\n') == NULL);
    assert(strstr(buf, "line1 line2") != NULL);

    tool_audit_mask(NULL, buf, sizeof(buf));
    assert(buf[0] == '\0');
    tool_audit_mask("{}", buf, 2);
    assert(buf[0] == '\0');
}

static void test_sum(void)
{
    char buf[64];
    char big[512];

    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    tool_audit_sum(big, buf, sizeof(buf));
    assert(strlen(buf) == sizeof(buf) - 1);
    assert(strcmp(buf + strlen(buf) - 3, "...") == 0);

    tool_audit_sum("ok\n", buf, sizeof(buf));
    assert(strcmp(buf, "ok ") == 0);

    tool_audit_sum(NULL, buf, sizeof(buf));
    assert(buf[0] == '\0');
}

static void test_sbox(void)
{
    const char *s = tool_audit_sbox();

    assert(s != NULL);
    assert(strcmp(s, "landlock") == 0 || strcmp(s, "degraded") == 0 || strcmp(s, "off") == 0);
}

int main(void)
{
    test_risk();
    test_mask();
    test_sum();
    test_sbox();
    printf("test_tool_audit: all checks passed\n");
    return 0;
}
