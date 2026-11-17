// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_priv_exec.c
 * @brief C-22 特权执行通道注入测试：白名单、参数越权、通道开关、非 root
 *        拒绝与审计落盘。
 *
 * 授权的真实安装（dnf install tree）需要 root 与网络，实验室环境不成立，
 * 此处只覆盖可在非 root 下复现的拒绝面与审计面；成功执行记为未验证。
 */

#include "builtin/builtin_priv_internal.h"

#include "airy_memory.h"
#include "daemon_security.h"
#include "error_codes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_switch(const char *v)
{
    if (v)
        setenv(PRIV_ENV_SWITCH, v, 1);
    else
        unsetenv(PRIV_ENV_SWITCH);
}

static void clear_result(tool_result_t *res)
{
    AIRY_FREE(res->output);
    AIRY_FREE(res->error);
    AIRY_MEMSET(res, 0, sizeof(*res));
}

static int build_rc(const char *json)
{
    tool_result_t res;
    priv_plan_t plan;
    AIRY_MEMSET(&res, 0, sizeof(res));
    int rc = priv_plan_build(json, &plan, &res);
    if (rc == AIRY_OK) {
        assert(plan.program != NULL);
        assert(plan.argc >= 1);
        assert(plan.argv[plan.argc + 1] == NULL);
        assert(strcmp(plan.argv[0], plan.program->name) == 0);
    }
    priv_plan_free(&plan);
    clear_result(&res);
    return rc;
}

static void test_switch(void)
{
    set_switch(NULL);
    assert(priv_enabled() == 1);
    set_switch("on");
    assert(priv_enabled() == 1);
    const char *off[] = {"off", "OFF", "0", "false", "No", NULL};
    for (int i = 0; off[i]; ++i) {
        set_switch(off[i]);
        assert(priv_enabled() == 0);
    }
    set_switch(NULL);
    printf("PASS switch\n");
}

static void test_lookup(void)
{
    const priv_prog_t *p = priv_lookup("dnf");
    assert(p != NULL && p->path[0] == '/' && p->family == PRIV_FAMILY_PKG);
    assert(priv_lookup("systemctl") != NULL);
    assert(priv_lookup("bash") == NULL);
    assert(priv_lookup("") == NULL);
    assert(priv_lookup(NULL) == NULL);
    /* 路径形式不接受：模型只能给短名，避免绕过白名单表。 */
    assert(priv_lookup("/usr/bin/dnf") == NULL);
    assert(priv_lookup("../bin/dnf") == NULL);
    printf("PASS lookup\n");
}

static void test_plan(void)
{
    assert(build_rc("{\"program\":\"dnf\",\"args\":[\"install\",\"-y\",\"tree\"]}") == AIRY_OK);
    assert(build_rc("{\"program\":\"rpm\",\"args\":[\"-q\",\"tree\"]}") == AIRY_OK);
    assert(build_rc("{\"program\":\"systemctl\",\"args\":[\"status\",\"sshd\"]}") == AIRY_OK);

    /* 白名单外程序与路径注入 */
    assert(build_rc("{\"program\":\"bash\",\"args\":[\"-c\",\"id\"]}") ==
           AIRY_ERR_EXEC_NOT_FOUND);
    assert(build_rc("{\"program\":\"../../bin/bash\",\"args\":[\"-c\",\"id\"]}") ==
           AIRY_ERR_EXEC_NOT_FOUND);

    /* 参数越权：shell 元字符 / 本地路径 / 表外选项 / 缺动词 / 超量 */
    assert(build_rc("{\"program\":\"dnf\",\"args\":[\"install\",\"tree; rm -rf /\"]}") ==
           AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"dnf\",\"args\":[\"install\",\"/tmp/x.rpm\"]}") ==
           AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"dnf\",\"args\":[\"install\",\"--installroot=/tmp/r\"]}") ==
           AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"dnf\",\"args\":[\"tree\"]}") == AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"systemctl\",\"args\":[\"reboot\"]}") == AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"dnf\",\"args\":[]}") == AIRY_ERR_EXEC_ARGS);

    /* 参数规模上界与协议错 */
    char big[2048];
    size_t off = (size_t)snprintf(big, sizeof(big), "{\"program\":\"dnf\",\"args\":[\"install\"");
    for (int i = 0; i < PRIV_MAX_ARGS; ++i)
        off += (size_t)snprintf(big + off, sizeof(big) - off, ",\"p%d\"", i);
    snprintf(big + off, sizeof(big) - off, "]}");
    assert(build_rc(big) == AIRY_ERR_EXEC_ARGS);
    assert(build_rc("{\"program\":\"dnf\",\"args\":[}") == AIRY_ERR_PARSE_ERROR);
    assert(build_rc("{}") == AIRY_ERR_INVALID_PARAM);
    printf("PASS plan\n");
}

static void init_audit(char *dir, size_t cap)
{
    snprintf(dir, cap, "/tmp/airy_priv_audit_%ld", (long)getpid());
    mkdir(dir, 0700);
    daemon_security_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.enable_audit_logging = true;
    cfg.audit_log_dir = dir;
    assert(daemon_security_init(&cfg, NULL) == 0);
}

static int audit_has(const char *dir, const char *needle)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/daemon_audit.log", dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void test_exec_refusals(void)
{
    char dir[256];
    init_audit(dir, sizeof(dir));

    tool_result_t res;
    AIRY_MEMSET(&res, 0, sizeof(res));

    /* 通道关闭：先于一切判定拒绝。 */
    set_switch("off");
    assert(priv_exec_tool("{\"program\":\"dnf\",\"args\":[\"install\",\"tree\"]}", 0, &res) ==
           AIRY_ERR_EXEC_PERMISSION);
    assert(res.err_code == AIRY_ERR_EXEC_PERMISSION);
    clear_result(&res);
    set_switch("on");

    /* 白名单外程序：不因 root 与否而改变。 */
    assert(priv_exec_tool("{\"program\":\"bash\",\"args\":[\"-c\",\"id\"]}", 0, &res) ==
           AIRY_ERR_EXEC_NOT_FOUND);
    assert(res.err_code == AIRY_ERR_EXEC_NOT_FOUND);
    clear_result(&res);

    if (geteuid() != 0) {
        assert(priv_exec_tool("{\"program\":\"dnf\",\"args\":[\"install\",\"tree\"]}", 0, &res) ==
               AIRY_ERR_EXEC_PERMISSION);
        assert(res.err_code == AIRY_ERR_EXEC_PERMISSION);
        clear_result(&res);
        printf("PASS exec-refusal (non-root)\n");
    } else {
        printf("SKIP exec-refusal non-root branch (running as root; not executing install)\n");
    }

    assert(audit_has(dir, "privileged_exec"));
    assert(audit_has(dir, "FAILED"));
    printf("PASS audit\n");

    char path[320];
    snprintf(path, sizeof(path), "%s/daemon_audit.log", dir);
    unlink(path);
    rmdir(dir);
    daemon_security_shutdown();
}

int main(void)
{
    test_switch();
    test_lookup();
    test_plan();
    test_exec_refusals();
    printf("ALL PASS test_priv_exec\n");
    return 0;
}
