// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_config_decl.c
 * @brief 0.1.20 B-A7 A7-4 声明面回归（model.yaml provision/artifact/
 *        capabilities）
 *
 * 锁定五条口径：
 *   1. capabilities 显式声明为唯一来源：整表替换，显式 false 即 false，
 *      未声明位恒 false；provider 级取首个显式声明的模型行
 *   2. 无 capabilities 声明时 8 位全 false（不派生、不按厂商名猜测）
 *   3. models[] 行未知键 fail-closed：该行拒载，其余行不受影响
 *   4. provision: managed 与 artifact 路径落位 model_entry_t
 *   5. provision 值域外（bogus）与 capabilities 裸名外键均拒载该行
 */

#include "config/types.h"
#include "providers/core/provider_config.h"

#include "airy_memory.h"
#include "error.h"

#include <stdio.h>
#include <string.h>

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

static void check_bit(const char *name, bool got, bool want)
{
    check_int(name, got ? 1 : 0, want ? 1 : 0);
}

static void free_providers(provider_config_t *provs, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        AIRY_FREE(provs[i].name);
        AIRY_FREE(provs[i].api_format);
        AIRY_FREE(provs[i].api_key);
        AIRY_FREE(provs[i].api_base);
        AIRY_FREE(provs[i].extra_headers);
        if (provs[i].models) {
            for (size_t k = 0; provs[i].models[k]; ++k)
                AIRY_FREE(provs[i].models[k]);
            AIRY_FREE(provs[i].models);
        }
        AIRY_FREE(provs[i].model_max_output);
        AIRY_FREE(provs[i].model_max_concurrent);
    }
    AIRY_FREE(provs);
}

static int put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

/* t1：显式声明整表替换 + provider 级首个显式声明生效。 */
static const char kOverride[] = "models:\n"
                                "  - name: m1\n"
                                "    model_id: m1\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n"
                                "    capabilities:\n"
                                "      seed: false\n"
                                "      num_ctx: true\n"
                                "  - name: m2\n"
                                "    model_id: m2\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n"
                                "    capabilities:\n"
                                "      response_format: true\n";

/* t2：无 capabilities 声明：8 位全 false。 */
static const char kFallback[] = "models:\n"
                                "  - name: m1\n"
                                "    model_id: m1\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n";

/* t3：未知键拒载该行，有效行不受影响。 */
static const char kUnknownKey[] = "models:\n"
                                  "  - name: good\n"
                                  "    model_id: good\n"
                                  "    provider: prov\n"
                                  "    base_url: http://127.0.0.1:9/v1\n"
                                  "  - name: bad\n"
                                  "    model_id: bad\n"
                                  "    provider: prov\n"
                                  "    base_url: http://127.0.0.1:9/v1\n"
                                  "    apikey: leaked\n";

/* t4：provision/artifact 落位。 */
static const char kProvision[] = "models:\n"
                                 "  - name: local-qwen\n"
                                 "    model_id: qwen2\n"
                                 "    provider: prov\n"
                                 "    base_url: http://127.0.0.1:11435/v1\n"
                                 "    provision: managed\n"
                                 "    artifact: /var/airy/models/qwen2.gguf\n";

/* t5：provision 值域外拒载。 */
static const char kBadProvision[] = "models:\n"
                                    "  - name: good\n"
                                    "    model_id: good\n"
                                    "    provider: prov\n"
                                    "    base_url: http://127.0.0.1:9/v1\n"
                                    "  - name: bad\n"
                                    "    model_id: bad\n"
                                    "    provider: prov\n"
                                    "    base_url: http://127.0.0.1:9/v1\n"
                                    "    provision: serverfarm\n";

/* t6：capabilities 裸名外键拒载。 */
static const char kBadBit[] = "models:\n"
                              "  - name: good\n"
                              "    model_id: good\n"
                              "    provider: prov\n"
                              "    base_url: http://127.0.0.1:9/v1\n"
                              "  - name: bad\n"
                              "    model_id: bad\n"
                              "    provider: prov\n"
                              "    base_url: http://127.0.0.1:9/v1\n"
                              "    capabilities:\n"
                              "      seed: true\n"
                              "      ctx_len: true\n";

int main(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/airy_llm_decl_%ld.yaml", (long)getpid());

    printf("running config declaration surface (A7-4) regression\n");

    /* 1) 显式覆盖：整表替换 + 首个显式声明生效（m2 的 response_format
     *    不得反超 m1 的声明）。 */
    provider_config_t *provs = NULL;
    size_t n = 0;
    if (put_file(path, kOverride) != 0)
        return 1;
    check_int("override.rc", svc_load_model_config_yaml(path, &provs, &n), 0);
    check_int("override.count", (int)n, 1);
    if (provs && n == 1) {
        check_bit("override.seed", provs[0].caps.accepts_seed, false);
        check_bit("override.num_ctx", provs[0].caps.accepts_num_ctx, true);
        check_bit("override.rf", provs[0].caps.accepts_response_format, false);
        check_bit("override.grammar", provs[0].caps.accepts_grammar, false);
        free_providers(provs, n);
    }

    /* 2) 无声明：8 位全 false（未声明即不发，fail-safe）。 */
    provs = NULL;
    n = 0;
    if (put_file(path, kFallback) != 0)
        return 1;
    check_int("undecl.rc", svc_load_model_config_yaml(path, &provs, &n), 0);
    check_int("undecl.count", (int)n, 1);
    if (provs && n == 1) {
        check_bit("undecl.seed", provs[0].caps.accepts_seed, false);
        check_bit("undecl.grammar", provs[0].caps.accepts_grammar, false);
        check_bit("undecl.num_ctx", provs[0].caps.accepts_num_ctx, false);
        check_bit("undecl.rf", provs[0].caps.accepts_response_format, false);
        check_bit("undecl.cache_prompt", provs[0].caps.accepts_cache_prompt, false);
        check_bit("undecl.slot_id", provs[0].caps.accepts_slot_id, false);
        check_bit("undecl.thinking", provs[0].caps.accepts_enable_thinking, false);
        check_bit("undecl.budget", provs[0].caps.accepts_thinking_budget, false);
        free_providers(provs, n);
    }

    /* 3) 未知键：bad 行拒载，good 行照常导出。 */
    provs = NULL;
    n = 0;
    if (put_file(path, kUnknownKey) != 0)
        return 1;
    check_int("unknown.rc", svc_load_model_config_yaml(path, &provs, &n), 0);
    check_int("unknown.count", (int)n, 1);
    if (provs && n == 1) {
        check_str("unknown.model", provs[0].models[0], "good");
        free_providers(provs, n);
    }

    /* 4) provision/artifact 落位 model_entry_t。 */
    {
        if (put_file(path, kProvision) != 0)
            return 1;
        svc_yaml_state_t st;
        memset(&st, 0, sizeof(st));
        check_int("decl.rc", svc_yaml_load_state(path, &st), 0);
        check_int("decl.count", (int)st.model_count, 1);
        check_str("decl.provision", st.models[0].provision, "managed");
        check_str("decl.artifact", st.models[0].artifact, "/var/airy/models/qwen2.gguf");
    }

    /* 5) provision 值域外：bad 行拒载。 */
    {
        if (put_file(path, kBadProvision) != 0)
            return 1;
        svc_yaml_state_t st;
        memset(&st, 0, sizeof(st));
        check_int("badprov.rc", svc_yaml_load_state(path, &st), 0);
        check_int("badprov.count", (int)st.model_count, 1);
        check_str("badprov.kept", st.models[0].name, "good");
    }

    /* 6) capabilities 裸名外键：bad 行拒载。 */
    {
        if (put_file(path, kBadBit) != 0)
            return 1;
        svc_yaml_state_t st;
        memset(&st, 0, sizeof(st));
        check_int("badbit.rc", svc_yaml_load_state(path, &st), 0);
        check_int("badbit.count", (int)st.model_count, 1);
        check_str("badbit.kept", st.models[0].name, "good");
    }

    /* 7) P-A：文件不可读必须 fail-closed，不得回 0 冒充「加载成功但无
     *    provider」——后者会让启动与热重载都静默带着空注册表继续。 */
    {
        char missing[256];
        snprintf(missing, sizeof(missing), "/tmp/airy_llm_nocfg_%ld.yaml", (long)getpid());
        provider_config_t *mp = NULL;
        size_t mn = 99;
        check_int("noent.rc", svc_load_model_config_yaml(missing, &mp, &mn),
                  AIRY_ERR_PARSE_ERROR);
        check_int("noent.count", (int)mn, 0);
    }

    /* 8) P-A：可读但零模型零 provider 属装配失败（半写/空表），同样
     *    不得以成功语义流出。 */
    {
        if (put_file(path, "") != 0)
            return 1;
        provider_config_t *ep = NULL;
        size_t en = 99;
        check_int("empty.rc", svc_load_model_config_yaml(path, &ep, &en), AIRY_ERR_STATE_ERROR);
        check_int("empty.count", (int)en, 0);
    }

    remove(path);

    if (fail_count == 0) {
        printf("All %d decl assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d decl assertions failed\n", fail_count, test_count);
    return 1;
}
