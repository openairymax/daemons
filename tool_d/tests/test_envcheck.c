// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_envcheck.c
 * @brief C-26 环境可行性预检注入测试：开关六值、探针事实、命令存在性、
 *        七类确定标记的判定与「Nothing to do」等普通失败的不误判。
 *
 * 判定面全部是纯函数与只读探针，可在非 root、无网络环境复现；
 * 真实「不存在的包」经 dnf 的端到端复验记 gate:lab。
 */

#include "builtin/builtin_envcheck_internal.h"

#include "airy_memory.h"
#include "error_codes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set_switch(const char *v)
{
    if (v)
        setenv(ENV_PRECHECK_SWITCH, v, 1);
    else
        unsetenv(ENV_PRECHECK_SWITCH);
}

static void test_switch(void)
{
    set_switch(NULL);
    assert(env_precheck_enabled() == 1);
    set_switch("on");
    assert(env_precheck_enabled() == 1);
    const char *off[] = {"off", "OFF", "0", "false", "No", NULL};
    for (int i = 0; off[i]; ++i) {
        set_switch(off[i]);
        assert(env_precheck_enabled() == 0);
    }
    set_switch(NULL);
    printf("PASS switch\n");
}

static void test_probe(void)
{
    env_probe_t p;
    assert(env_probe_collect(&p) == AIRY_OK);
    assert(p.distro[0] != '\0');
    assert(p.version[0] != '\0');
    assert(p.pkg_mgr[0] != '\0');
    assert(p.repos_ready == 0 || p.repos_ready == 1);
    assert(p.has_rpm == 0 || p.has_rpm == 1);
    assert(env_probe_collect(NULL) == AIRY_ERR_INVALID_PARAM);
    printf("PASS probe\n");
}

static void test_program(void)
{
    assert(env_probe_program("sh") == 1);
    assert(env_probe_program("/bin/sh") == 1);
    assert(env_probe_program("airy-no-such-cmd-xyz") == 0);
    assert(env_probe_program("/nonexistent/airy-no-such-cmd-xyz") == 0);
    assert(env_probe_program("") == 0);
    assert(env_probe_program(NULL) == 0);
    printf("PASS program\n");
}

static void test_classify(void)
{
    env_probe_t p;
    AIRY_MEMSET(&p, 0, sizeof(p));
    snprintf(p.distro, sizeof(p.distro), "fedora");
    snprintf(p.version, sizeof(p.version), "40");
    snprintf(p.pkg_mgr, sizeof(p.pkg_mgr), "dnf");
    p.repos_ready = 1;

    char reason[ENV_REASON_MAX];
    const char *marks[] = {
        "Error: No match for argument: wemeet",
        "Error: Unable to find a match: wemeet",
        "No package wemeet available.",
        "sh: wemeet: command not found",
        "Could not resolve host: mirrors.example.com",
        "Failed to download metadata for repo 'fedora'",
        "Cannot download packages whilst offline",
        NULL,
    };
    for (int i = 0; marks[i]; ++i) {
        assert(env_classify(1, marks[i], &p, reason, sizeof(reason)) == AIRY_ERR_EXEC_ENV);
        assert(strstr(reason, "environment unmet") != NULL);
        assert(strstr(reason, "distro=fedora") != NULL);
        assert(strstr(reason, "version=40") != NULL);
        assert(strstr(reason, "pkg_mgr=dnf") != NULL);
        assert(strstr(reason, "repos=ready") != NULL);
    }

    /* 普通失败不得误判：Nothing to do 是「已满足」而不是环境不可行。 */
    assert(env_classify(1, "Nothing to do.", &p, reason, sizeof(reason)) == AIRY_OK);
    assert(env_classify(1, "Permission denied", &p, reason, sizeof(reason)) == AIRY_OK);
    assert(env_classify(0, "No match for argument: x", &p, reason, sizeof(reason)) == AIRY_OK);
    assert(env_classify(1, NULL, &p, reason, sizeof(reason)) == AIRY_OK);
    assert(env_classify(1, "", &p, reason, sizeof(reason)) == AIRY_OK);
    assert(env_classify(1, "No match for argument: x", &p, NULL, 0) == AIRY_OK);

    /* 探针缺失时判定照常成立，归因写 unknown 而不是留空。 */
    assert(env_classify(1, "No match for argument: x", NULL, reason, sizeof(reason)) ==
           AIRY_ERR_EXEC_ENV);
    assert(strstr(reason, "distro=unknown") != NULL);
    assert(strstr(reason, "repos=unreadable") != NULL);
    printf("PASS classify\n");
}

int main(void)
{
    test_switch();
    test_probe();
    test_program();
    test_classify();
    printf("ALL PASS test_envcheck\n");
    return 0;
}
