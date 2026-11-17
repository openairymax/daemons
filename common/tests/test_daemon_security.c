// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_daemon_security.c - Daemon Security Module Unit Tests
 */

#include "../include/daemon_security.h"
#include "../src/security/daemon_security_internal.h"

#include "airy_dirent.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define AIRY_TEST_MKDIR(p) _mkdir(p)
#define AIRY_TEST_SETENV(k, v) _putenv_s((k), (v))
#define AIRY_TEST_PID ((int)_getpid())
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define AIRY_TEST_MKDIR(p) mkdir((p), 0700)
#define AIRY_TEST_SETENV(k, v) setenv((k), (v), 1)
#define AIRY_TEST_PID ((int)getpid())
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)               \
    do {                         \
        tests_run++;             \
        printf("  %-50s", name); \
    } while (0)
#define PASS()              \
    do {                    \
        tests_passed++;     \
        printf("[PASS]\n"); \
    } while (0)
#define FAIL(msg)                   \
    do {                            \
        printf("[FAIL] %s\n", msg); \
        return;                     \
    } while (0)
#define ASSERT(cond, msg) \
    do {                  \
        if (!(cond)) {    \
            FAIL(msg);    \
        }                 \
    } while (0)

/* P-F：夹具必须自带安装根。daemon_security_init() 未给 audit_log_dir 时
 * 把审计路径回落为 $AIRY_HOME/data/agentrt/logs/daemon_audit.log；此前
 * 夹具直写宿主 ~/.airymaxrt，单测事件混进真实审计日志（r86 取证：493 条
 * FAILED 全部来自本文件）。此处以 PID 建隔离根并预建日志目录，事件只落
 * 在临时根内，审计文件本身也可以被断言。
 * 注意：必须在任何 airy_* 路径解析之前设置 AIRY_HOME——路径系统首次
 * 调用即缓存解析结果，晚设置不会生效。 */
static char g_home[512];

static int t_mkdir(const char *path)
{
    return (AIRY_TEST_MKDIR(path) != 0 && errno != EEXIST) ? -1 : 0;
}

static void t_audit_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/data/agentrt/logs/daemon_audit.log", g_home);
}

static int t_home_init(void)
{
    const char *base = getenv("TMPDIR");
    char dir[600];

    if (!base || base[0] == '\0')
        base = getenv("TEMP");
    if (!base || base[0] == '\0')
        base = "/tmp";
    snprintf(g_home, sizeof(g_home), "%s/svc_sec_%d", base, AIRY_TEST_PID);
    if (t_mkdir(g_home) != 0)
        return -1;
    snprintf(dir, sizeof(dir), "%s/data", g_home);
    if (t_mkdir(dir) != 0)
        return -1;
    snprintf(dir, sizeof(dir), "%s/data/agentrt", g_home);
    if (t_mkdir(dir) != 0)
        return -1;
    snprintf(dir, sizeof(dir), "%s/data/agentrt/logs", g_home);
    if (t_mkdir(dir) != 0)
        return -1;
    t_audit_path(dir, sizeof(dir));
    remove(dir);
    return AIRY_TEST_SETENV("AIRY_HOME", g_home) == 0 ? 0 : -1;
}

static int t_file_has(const char *path, const char *needle)
{
    char buf[4096];
    FILE *fp = fopen(path, "r");

    if (!fp)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void test_init_null_config(void)
{
    TEST("Init with NULL config");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "init with NULL config should succeed");
    daemon_security_shutdown();
    PASS();
}

static void test_double_init(void)
{
    TEST("Double init is idempotent");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "first init");
    ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "second init should be idempotent");
    daemon_security_shutdown();
    PASS();
}

static void test_shutdown_without_init(void)
{
    TEST("Shutdown without init is safe");
    daemon_security_shutdown();
    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_normal(void)
{
    TEST("Sanitize tool params - normal");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char sanitized_tool[128];
    char sanitized_params[256];
    int ret = daemon_sanitize_tool_params("read_file", "{\"path\":\"/tmp/test\"}", sanitized_tool,
                                          sizeof(sanitized_tool), sanitized_params,
                                          sizeof(sanitized_params));
    ASSERT(ret == 0, "sanitize tool params should succeed");
    ASSERT(strstr(sanitized_tool, "read_file") != NULL, "tool name should be preserved");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_dangerous(void)
{
    TEST("Sanitize tool params - shell-like data is not rejected (0.1.14)");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char sanitized_tool[128];
    char sanitized_params[256];
    /* 0.1.14 语义变更：SEC-014 注入模式子串扫描已移除——工具参数是数据，
     * 含 ; | > 等字符的合法数据（fs_write 写 HTML、grep 正则）此前被整段
     * 误杀（0.1.13 实机回归）。sanitize 层仅做控制字符剥离留痕（审计/展示），
     * 拒绝职责由 ACL + tool_approval + 沙箱配额承担（白名单化记入 0.2.x）。 */
    int ret = daemon_sanitize_tool_params("exec", "; cat /etc/passwd", sanitized_tool,
                                          sizeof(sanitized_tool), sanitized_params,
                                          sizeof(sanitized_params));
    ASSERT(ret == 0, "tool params are data: shell-like content must not be rejected here");
    ASSERT(strchr(sanitized_params, ';') != NULL, "sanitized copy keeps displayable content");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_null(void)
{
    TEST("Sanitize tool params - null params rejected");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char buf[64];
    int ret = daemon_sanitize_tool_params(NULL, "{}", buf, 64, buf, 64);
    ASSERT(ret != 0, "null tool_name should be rejected");

    ret = daemon_sanitize_tool_params("tool", NULL, buf, 64, buf, 64);
    ASSERT(ret != 0, "null params should be rejected");

    daemon_security_shutdown();
    PASS();
}

static void test_check_tool_permission_no_acl(void)
{
    TEST("Check tool permission - no ACL entry denies");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int allowed = daemon_check_tool_permission("agent_001", "file_read", "execute");
    ASSERT(allowed != 0, "no ACL entry should deny by default");

    daemon_security_shutdown();
    PASS();
}

static void test_check_tool_permission_null_params(void)
{
    TEST("Check tool permission - null params denied");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int allowed = daemon_check_tool_permission(NULL, "tool", "execute");
    ASSERT(allowed != 0, "null agent_id should be denied");

    allowed = daemon_check_tool_permission("agent", NULL, "execute");
    ASSERT(allowed != 0, "null tool_name should be denied");

    allowed = daemon_check_tool_permission("agent", "tool", NULL);
    ASSERT(allowed != 0, "null action should be denied");

    daemon_security_shutdown();
    PASS();
}

static void test_audit_log_event(void)
{
    TEST("Audit log event");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int ret = daemon_audit_log_event("tool_d", "execute", "/tool/read_file", 0, "agent_001");
    ASSERT(ret == 0, "audit success event should succeed");

    ret = daemon_audit_log_event("llm_d", "query_failed", "/llm/gpt-4", -1, "agent_002");
    ASSERT(ret == 0, "audit failure event should succeed");

    daemon_security_shutdown();

    /* 审计事件必须真实落盘到隔离安装根，而不是只返回成功码 */
    char audit[600];
    t_audit_path(audit, sizeof(audit));
    ASSERT(t_file_has(audit, "operation=query_failed"),
           "audit log must contain the failure event on disk");
    PASS();
}

static void test_audit_log_event_null_params(void)
{
    TEST("Audit log event - null params handled");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int ret = daemon_audit_log_event(NULL, "op", "res", 0, "agent");
    ASSERT(ret != 0, "null service_name should be rejected");

    ret = daemon_audit_log_event("svc", NULL, "res", 0, "agent");
    ASSERT(ret != 0, "null operation should be rejected");

    daemon_security_shutdown();
    PASS();
}

static void test_init_with_config(void)
{
    TEST("Init with explicit config");
    daemon_security_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sanitize_level = SANITIZE_LEVEL_HIGH;
    cfg.enable_permission_cache = true;
    cfg.enable_vault = true;
    cfg.enable_audit_logging = true;

    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(&cfg, &err);
    ASSERT(ret == 0, "init with config should succeed");

    daemon_security_shutdown();
    PASS();
}

static void test_rules_file_loading(void)
{
    TEST("Load tool permission rules file");
    /* CI-2：PID 隔离，避免 ctest -j 并行互踩；文件落在隔离安装根内，
     * 不再假设 Windows 上存在 /tmp。 */
    char path[600];
    snprintf(path, sizeof(path), "%s/rules_%d.yaml", g_home, AIRY_TEST_PID);
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "cannot create temp rules file");
    fputs("rules:\n"
          "  - agent: \"coding_v1\"\n"
          "    tool: \"fs_read\"\n"
          "    effect: \"allow\"\n"
          "  - agent: \"coding_v1\"\n"
          "    tool: \"shell_run\"\n"
          "    effect: \"deny\"\n",
          fp);
    fclose(fp);

    daemon_security_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sanitize_level = SANITIZE_LEVEL_HIGH;
    cfg.enable_permission_cache = true;
    cfg.enable_vault = true;
    cfg.enable_audit_logging = true;
    cfg.permission_rules_path = path;

    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(&cfg, &err);
    ASSERT(ret == 0, "init with rules file should succeed");

    ASSERT(daemon_check_tool_permission("coding_v1", "fs_read", "execute") == AIRY_OK,
           "granted tool should be allowed");
    ASSERT(daemon_check_tool_permission("coding_v1", "shell_run", "execute") != AIRY_OK,
           "denied tool should be blocked");
    ASSERT(daemon_check_tool_permission("coding_v1", "web_search", "execute") != AIRY_OK,
           "unlisted tool should be denied (fail-closed)");

    daemon_security_shutdown();
    remove(path);
    PASS();
}

/* ---- A-3：审计轮转契约用例 ---- */
static unsigned t_scan_backups(char *newest, size_t cap)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/data/agentrt/logs", g_home);
    unsigned count = 0;
    if (newest && cap) {
        newest[0] = '\0';
    }
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "daemon_audit.log.", 17) != 0) {
            continue;
        }
        count++;
        if (newest && cap) {
            snprintf(newest, cap, "%s/%s", dir, ent->d_name);
        }
    }
    closedir(d);
    return count;
}

static void t_clear_rot_env(void)
{
    AIRY_TEST_SETENV("AIRY_AUDIT_MAX_BYTES", "");
    AIRY_TEST_SETENV("AIRY_AUDIT_KEEP_FILES", "");
    AIRY_TEST_SETENV("AIRY_AUDIT_COMPRESS", "");
    AIRY_TEST_SETENV("AIRY_AUDIT_ENCRYPT", "");
}

static void t_clean_backups(void)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/data/agentrt/logs", g_home);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "daemon_audit.log.", 17) == 0) {
            char victim[900];
            snprintf(victim, sizeof(victim), "%s/%s", dir, ent->d_name);
            remove(victim);
        }
    }
    closedir(d);
}

static void test_audit_policy_contract(void)
{
    TEST("audit policy equals contract (500MB/365)");
    audit_rot_policy_t p;
    aud_rot_policy(&p);
    ASSERT(p.max_bytes == AUD_MAX_BYTES_DEF, "default cap must be 500MB");
    ASSERT(p.keep_files == AUD_KEEP_FILES_DEF, "default keep must be 365");
    ASSERT(p.max_bytes == 500ULL * 1024ULL * 1024ULL, "500MB literal");
    PASS();
}

static void test_audit_rotate_size(void)
{
    TEST("audit rotate on size cap and restore backup");
    airy_err_t err = AIRY_OK;
    t_clean_backups();
    AIRY_TEST_SETENV("AIRY_AUDIT_MAX_BYTES", "2048");
    AIRY_TEST_SETENV("AIRY_AUDIT_KEEP_FILES", "3");
    ASSERT(daemon_security_init(NULL, &err) == 0, "init should succeed");
    for (int i = 0; i < 40; i++) {
        (void)daemon_audit_log_event("svc", "rotate_probe", "/r", 0, "agent");
    }
    daemon_security_shutdown();

    char path[600];
    t_audit_path(path, sizeof(path));
    struct stat st;
    ASSERT(stat(path, &st) == 0, "main audit file must exist");
    ASSERT((unsigned long long)st.st_size <= 2048ULL, "main file must fall back below cap");

    char backup[700];
    unsigned n = t_scan_backups(backup, sizeof(backup));
    ASSERT(n >= 1, "rotation must produce a backup");
    ASSERT(backup[0] != '\0', "backup path must be found");

    audit_rot_policy_t p;
    aud_rot_policy(&p);
    if (p.compress) {
        ASSERT(strstr(backup, ".zst") != NULL, "zstd build must emit .zst backup");
    }
    if (p.encrypt) {
        ASSERT(strstr(backup, ".enc") != NULL, "openssl build must emit .enc backup");
    }

    char out[700];
    snprintf(out, sizeof(out), "%s/restored.log", g_home);
    ASSERT(aud_rot_restore(backup, out) == 0, "backup must decrypt/decompress");
    ASSERT(t_file_has(out, "operation=rotate_probe"), "restored backup carries events");
    remove(out);
    t_clear_rot_env();
    PASS();
}

static void test_audit_rotate_keep(void)
{
    TEST("audit rotation respects keep_files");
    airy_err_t err = AIRY_OK;
    t_clean_backups();
    AIRY_TEST_SETENV("AIRY_AUDIT_MAX_BYTES", "512");
    AIRY_TEST_SETENV("AIRY_AUDIT_KEEP_FILES", "2");
    ASSERT(daemon_security_init(NULL, &err) == 0, "init should succeed");
    for (int i = 0; i < 60; i++) {
        (void)daemon_audit_log_event("svc", "keep_probe", "/r", 0, "agent");
    }
    daemon_security_shutdown();
    unsigned n = t_scan_backups(NULL, 0);
    ASSERT(n >= 1, "rotation must have happened");
    ASSERT(n <= 2, "keep_files=2 must bound backup count");
    t_clear_rot_env();
    PASS();
}

static void test_audit_rotate_disabled(void)
{
    TEST("audit rotation disabled by AIRY_AUDIT_MAX_BYTES=0");
    airy_err_t err = AIRY_OK;
    t_clean_backups();
    AIRY_TEST_SETENV("AIRY_AUDIT_MAX_BYTES", "0");
    ASSERT(daemon_security_init(NULL, &err) == 0, "init should succeed");
    for (int i = 0; i < 40; i++) {
        (void)daemon_audit_log_event("svc", "off_probe", "/r", 0, "agent");
    }
    daemon_security_shutdown();
    unsigned n = t_scan_backups(NULL, 0);
    ASSERT(n == 0, "no backup when rotation is disabled");
    t_clear_rot_env();
    PASS();
}

int main(void)
{
    printf("\n=== Daemon Security Module Unit Tests ===\n\n");

    if (t_home_init() != 0) {
        printf("[FAIL] cannot prepare isolated AIRY_HOME (%s)\n", g_home);
        return 1;
    }

    test_init_null_config();
    test_double_init();
    test_shutdown_without_init();
    test_sanitize_tool_params_normal();
    test_sanitize_tool_params_dangerous();
    test_sanitize_tool_params_null();
    test_check_tool_permission_no_acl();
    test_check_tool_permission_null_params();
    test_audit_log_event();
    test_audit_log_event_null_params();
    test_init_with_config();
    test_rules_file_loading();
    test_audit_policy_contract();
    test_audit_rotate_size();
    test_audit_rotate_keep();
    test_audit_rotate_disabled();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
