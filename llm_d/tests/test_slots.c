// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_slots.c
 * @brief 0.1.21 C-19 角色槽位（tc3）单源回归
 *
 * 锁定五条验收口径：
 *   1. 启动装载 slots: 段，get 同时给出 declared 与回落后的 effective
 *   2. set 只接受已注册 model_id；非法值拒绝并列出可选值
 *   3. set 成功即落盘（只改 slots 行，其余字节保留）并触发重载，get/
 *      list_models 与服务内视图一致
 *   4. 清空槽位 → degraded=true，effective 回落 default_model
 *   5. 悬空引用（配置里指向未注册模型）整次重载作废，旧槽位继续服务；
 *      旧 env 名仍可读且带 legacy 标记
 */

#include "llm_service.h"
#include "rpc/service.h"
#include "svc_model_defaults.h"

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count;
static int fail_count;

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (!got || strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got ? got : "(null)", want);
        fail_count++;
    }
}

static void check_has(const char *name, const char *hay, const char *needle)
{
    test_count++;
    if (!hay || !strstr(hay, needle)) {
        printf("    FAIL: %s: \"%s\" not found in %s\n", name, needle, hay ? hay : "(null)");
        fail_count++;
    }
}

static char g_root[256];
static char g_cfg[256];

static int put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* 两个注册模型 + 三个槽位（t2/t1f 指向 alpha，t1p 指向 mini）。 */
static const char kCfg[] = "global:\n"
                           "  default_model: alpha\n"
                           "  default_provider: pa\n"
                           "think:\n"
                           "  enabled: true\n"
                           "  timeout_ms: 9000\n"
                           "slots:\n"
                           "  t2: alpha\n"
                           "  t1f: alpha\n"
                           "  t1p: mini\n"
                           "models:\n"
                           "  - name: alpha\n"
                           "    model_id: alpha\n"
                           "    provider: pa\n"
                           "    base_url: http://127.0.0.1:9/v1\n"
                           "  - name: mini\n"
                           "    model_id: mini\n"
                           "    provider: pa\n"
                           "    base_url: http://127.0.0.1:9/v1\n";

int main(void)
{
    printf("running role slots (tc3) single-source regression\n");

    snprintf(g_root, sizeof(g_root), "/tmp/airy_llm_slots_%ld", (long)getpid());
    if (mkdir(g_root, 0700) != 0)
        return 1;
    /* 路径体系首次解析后缓存：配置目录与服务配置必须是同一份文件，
     * 才能覆盖「set 落盘 → reload 读回」的真实链路。 */
    char cfg_dir[256];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/config", g_root);
    if (mkdir(cfg_dir, 0700) != 0)
        return 1;
    setenv("AIRY_HOME", g_root, 1);
    setenv("AIRY_CONFIG_DIR", cfg_dir, 1);
    unsetenv("AIRY_MODEL_T2");
    unsetenv("AIRY_MODEL_T1F");
    unsetenv("AIRY_MODEL_T1P");
    unsetenv("AIRY_THINK2_SLOW_MODEL");
    unsetenv("AIRY_THINK1_FAST_MODEL");
    unsetenv("AIRY_THINK1_PROF_MODEL");
    snprintf(g_cfg, sizeof(g_cfg), "%s/model.yaml", cfg_dir);

    if (put_file(g_cfg, kCfg) != 0)
        return 1;

    llm_service_t *svc = llm_service_create(g_cfg);
    if (!svc) {
        printf("    FAIL: service create returned NULL\n");
        return 1;
    }

    /* 1) 启动装载：declared/effective 同源，无降级。 */
    char *js = llm_service_slots_get(svc);
    if (!js) {
        printf("    FAIL: slots_get returned NULL\n");
        return 1;
    }
    check_has("boot.t1p_declared", js, "\"t1p\":{\"declared\":\"mini\",\"effective\":\"mini\"}");
    check_has("boot.degraded_false", js, "\"degraded\":false");
    free(js);
    check_int("boot.slot_t1p", strcmp(svc->slots.t1p, "mini"), 0);
    check_int("boot.reload_ok", (int)svc->reload_ok, 0);

    /* 2) 非法 set：未注册模型明确拒绝并列出可选值。 */
    char err[512];
    int rc = llm_service_slots_set(svc, "t1f", "nope", err, sizeof(err));
    check_int("bad.rc", rc == AIRY_ERR_INVALID_PARAM, 1);
    check_has("bad.reason", err, "not in registry");
    check_has("bad.choices_alpha", err, "alpha");
    check_has("bad.choices_mini", err, "mini");
    rc = llm_service_slots_set(svc, "t9", "alpha", err, sizeof(err));
    check_int("bad_role.rc", rc == AIRY_ERR_INVALID_PARAM, 1);
    check_has("bad_role.reason", err, "unknown slot");

    /* 3) 合法 set：落盘 + 重载，get 与文件一致，注释与他段保留。 */
    rc = llm_service_slots_set(svc, "t1f", "mini", err, sizeof(err));
    check_int("set.rc", rc, 0);
    check_str("set.mem", svc->slots.t1f, "mini");
    js = llm_service_slots_get(svc);
    check_has("set.get_t1f", js ? js : "", "\"t1f\":{\"declared\":\"mini\",\"effective\":\"mini\"}");
    free(js);
    js = llm_service_list_models(svc);
    check_has("set.list_slots", js ? js : "", "\"slots\"");
    free(js);
    {
        const char *txt = slurp(g_cfg);
        check_has("set.file_t1f", txt, "t1f: \"mini\"");
        check_has("set.file_keep_think", txt, "timeout_ms: 9000");
        check_has("set.file_keep_t2", txt, "t2: \"alpha\"");
    }

    /* 4) 清空槽位：degraded 显式暴露，effective 回落 default_model。 */
    rc = llm_service_slots_set(svc, "t2", "", err, sizeof(err));
    check_int("clear.rc", rc, 0);
    js = llm_service_slots_get(svc);
    check_has("clear.degraded_true", js ? js : "", "\"degraded\":true");
    check_has("clear.effective", js ? js : "",
              "\"t2\":{\"declared\":\"\",\"effective\":\"alpha\"}");
    free(js);

    /* 5) 旧 env 名：仍可读，但必须带 legacy 标记（调用方据此告警）。 */
    setenv("AIRY_THINK2_SLOW_MODEL", "alpha", 1);
    {
        svc_model_slots_t s;
        memset(&s, 0, sizeof(s));
        check_int("legacy.load", svc_slot_resolve(g_cfg, &s), 0);
        check_str("legacy.value", s.t2, "alpha");
        check_int("legacy.mask", (int)((s.legacy_mask >> SVC_SLOT_T2) & 1u), 1);
    }
    unsetenv("AIRY_THINK2_SLOW_MODEL");

    /* 6) 悬空引用：重载整次作废，旧槽位继续服务。最后执行——
     *    失败会进入 30s 退避窗口，不影响上面各步。 */
    put_file(g_cfg, "global:\n"
                    "  default_model: alpha\n"
                    "  default_provider: pa\n"
                    "slots:\n"
                    "  t2: ghost\n"
                    "  t1f: mini\n"
                    "models:\n"
                    "  - name: alpha\n"
                    "    model_id: alpha\n"
                    "    provider: pa\n"
                    "    base_url: http://127.0.0.1:9/v1\n"
                    "  - name: mini\n"
                    "    model_id: mini\n"
                    "    provider: pa\n"
                    "    base_url: http://127.0.0.1:9/v1\n");
    rc = llm_service_reload(svc);
    check_int("dangling.rejected", rc == AIRY_ERR_STATE_ERROR, 1);
    check_str("dangling.keep_t1f", svc->slots.t1f, "mini");
    check_str("dangling.keep_t2", svc->slots.t2, "");
    check_int("dangling.fail", (int)svc->reload_fail, 1);

    llm_service_destroy(svc);
    printf("%s: %d checks, %d failures\n", fail_count ? "FAIL" : "PASS", test_count, fail_count);
    return fail_count ? 1 : 0;
}
