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
 *   3. models[] 行未知键逐键忽略并告警，行本身保留（r113 P-R：整行拒载
 *      会把多余声明键放大成零模型的启动崩溃循环）
 *   4. provision: managed 与 artifact 路径落位 model_entry_t
 *   5. provision 值域外（bogus）与 capabilities 裸名外键均拒载该行
 *   6. 条目数恰好填满映射容量的行（8 键 / 16 键）必须完整解析：映射数组
 *      始终保留末尾零槽，yaml_size() 不会越过分配块读取堆残留
 */

#include "config/types.h"
#include "airy_prov/provider_config.h"

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

static void check_float(const char *name, float got, float want)
{
    test_count++;
    if (got < want - 0.0005f || got > want + 0.0005f) {
        printf("    FAIL: %s: got %f, want %f\n", name, (double)got, (double)want);
        fail_count++;
    }
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
        AIRY_FREE((void *)provs[i].model_temperature);
        AIRY_FREE((void *)provs[i].model_top_p);
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

/* t3：未知键只忽略该键，行保留；相邻合法键照常落位。 */
static const char kUnknownKey[] = "models:\n"
                                  "  - name: good\n"
                                  "    model_id: good\n"
                                  "    provider: prov\n"
                                  "    base_url: http://127.0.0.1:9/v1\n"
                                  "  - name: bad\n"
                                  "    model_id: bad\n"
                                  "    provider: prov\n"
                                  "    base_url: http://127.0.0.1:9/v1\n"
                                  "    apikey: leaked\n"
                                  "    timeout_ms: 120000\n"
                                  "    max_concurrent: 3\n";

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

/* t7：8 键行——条目数正好等于映射初始容量（8）。 */
static const char kFull8[] = "models:\n"
                             "  - name: eight\n"
                             "    model_id: eight\n"
                             "    mode: local\n"
                             "    base_url: http://127.0.0.1:9/v1\n"
                             "    provision: managed\n"
                             "    artifact: /var/airy/models/eight.gguf\n"
                             "    context_window: 8k\n"
                             "    provider: prov\n";

/* t9：B-1 能力位完整声明（厂商名 / g 后缀 / 三值）。 */
static const char kHwCaps[] = "models:\n"
                              "  - name: hw\n"
                              "    model_id: hw\n"
                              "    provider: prov\n"
                              "    base_url: http://127.0.0.1:9/v1\n"
                              "    accelerator: intel\n"
                              "    vram_bytes: 8g\n"
                              "    max_context: 8192\n"
                              "    supports_stream: true\n"
                              "    supports_tools: false\n";

/* t10：B-1 部分声明 + 非法值：declared=1，非法键保持未知（不得写成 0
 *      冒充「显式不支持」）。 */
static const char kHwBad[] = "models:\n"
                             "  - name: hwbad\n"
                             "    model_id: hwbad\n"
                             "    provider: prov\n"
                             "    base_url: http://127.0.0.1:9/v1\n"
                             "    accelerator: bogus\n"
                             "    vram_bytes: nonsense\n"
                             "    supports_stream: \"\"\n"
                             "    supports_tools: unknown\n";

/* t8：16 键行——条目数正好等于扩容后的容量（16），且重复 capabilities
 *     出现在中间。线上 0.1.20 崩溃即此形态：扫描 provider 键时踩到映射
 *     数组末尾之外的堆残留。 */
static const char kFull16[] = "models:\n"
                              "  - name: sixteen\n"
                              "    model_id: sixteen\n"
                              "    mode: local\n"
                              "    api_format: openai\n"
                              "    base_url: http://127.0.0.1:9/v1\n"
                              "    capabilities:\n"
                              "      seed: true\n"
                              "    api_key_env: \"\"\n"
                              "    provision: managed\n"
                              "    capabilities:\n"
                              "      num_ctx: true\n"
                              "    context_window: 32k\n"
                              "    max_output: 4k\n"
                              "    tool_rounds: 70\n"
                              "    vision: true\n"
                              "    thinking: auto\n"
                              "    provider: prov\n";

/* 1)+2) 显式覆盖整表替换、首个声明生效；无声明则 8 位全 false。 */
static int case_caps(const char *path)
{
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
    return 0;
}

/* 3) 未知键只忽略该键，行保留，合法键照常生效。 */
static int case_unknown_key(const char *path)
{
    provider_config_t *provs = NULL;
    size_t n = 0;
    if (put_file(path, kUnknownKey) != 0)
        return 1;
    check_int("unknown.rc", svc_load_model_config_yaml(path, &provs, &n), 0);
    check_int("unknown.count", (int)n, 1);
    if (provs && n == 1) {
        check_str("unknown.model", provs[0].models[0], "good");
        check_str("unknown.kept", provs[0].models[1], "bad");
        check_int("unknown.tail", provs[0].models[2] == NULL, 1);
        check_int("unknown.conc", provs[0].model_max_concurrent[1], 3);
        free_providers(provs, n);
    }
    return 0;
}

/* 4)~6) provision/artifact 落位；值域外与 capabilities 裸名外键拒载该行。 */
static int case_provision(const char *path)
{
    if (put_file(path, kProvision) != 0)
        return 1;
    svc_yaml_state_t st;
    memset(&st, 0, sizeof(st));
    check_int("decl.rc", svc_yaml_load_state(path, &st), 0);
    check_int("decl.count", (int)st.model_count, 1);
    check_str("decl.provision", st.models[0].provision, "managed");
    check_str("decl.artifact", st.models[0].artifact, "/var/airy/models/qwen2.gguf");

    memset(&st, 0, sizeof(st));
    if (put_file(path, kBadProvision) != 0)
        return 1;
    check_int("badprov.rc", svc_yaml_load_state(path, &st), 0);
    check_int("badprov.count", (int)st.model_count, 1);
    check_str("badprov.kept", st.models[0].name, "good");

    memset(&st, 0, sizeof(st));
    if (put_file(path, kBadBit) != 0)
        return 1;
    check_int("badbit.rc", svc_yaml_load_state(path, &st), 0);
    check_int("badbit.count", (int)st.model_count, 1);
    check_str("badbit.kept", st.models[0].name, "good");
    return 0;
}

/* 7)+8) P-A：不可读必须 fail-closed；可读但空表属装配失败。 */
static int case_missing(const char *path)
{
    char missing[256];
    snprintf(missing, sizeof(missing), "/tmp/airy_llm_nocfg_%ld.yaml", (long)getpid());
    provider_config_t *mp = NULL;
    size_t mn = 99;
    check_int("noent.rc", svc_load_model_config_yaml(missing, &mp, &mn), AIRY_ERR_PARSE_ERROR);
    check_int("noent.count", (int)mn, 0);

    if (put_file(path, "") != 0)
        return 1;
    provider_config_t *ep = NULL;
    size_t en = 99;
    check_int("empty.rc", svc_load_model_config_yaml(path, &ep, &en), AIRY_ERR_STATE_ERROR);
    check_int("empty.count", (int)en, 0);
    return 0;
}

/* 9)+10) 容量边界行（8 键 / 16 键）与重复 capabilities 必须完整解析。 */
static int case_full_rows(const char *path)
{
    svc_yaml_state_t st;
    memset(&st, 0, sizeof(st));
    if (put_file(path, kFull8) != 0)
        return 1;
    check_int("full8.rc", svc_yaml_load_state(path, &st), 0);
    check_int("full8.count", (int)st.model_count, 1);
    check_str("full8.name", st.models[0].name, "eight");
    check_str("full8.provider", st.models[0].provider, "prov");
    check_str("full8.provision", st.models[0].provision, "managed");
    check_str("full8.cw", st.models[0].context_window, "8k");
    check_int("full8.caps", st.models[0].has_caps, 0);

    memset(&st, 0, sizeof(st));
    if (put_file(path, kFull16) != 0)
        return 1;
    check_int("full16.rc", svc_yaml_load_state(path, &st), 0);
    check_int("full16.count", (int)st.model_count, 1);
    check_str("full16.name", st.models[0].name, "sixteen");
    check_str("full16.provider", st.models[0].provider, "prov");
    check_str("full16.mode", st.models[0].mode, "local");
    check_str("full16.fmt", st.models[0].api_format, "openai");
    check_str("full16.cw", st.models[0].context_window, "32k");
    check_int("full16.maxout", st.models[0].max_output_tokens, 4096);
    check_int("full16.rounds", st.models[0].tool_rounds, 70);
    check_int("full16.vision", st.models[0].vision, 1);
    check_str("full16.think", st.models[0].thinking, "auto");
    check_int("full16.caps", st.models[0].has_caps, 1);
    check_bit("full16.seed", st.models[0].caps_override.accepts_seed, true);
    check_bit("full16.numctx", st.models[0].caps_override.accepts_num_ctx, false);
    return 0;
}

/* 11)+11b) B-1 声明位落 provider 级；仅 context_window 时派生容量。 */
static int case_hw_decl(const char *path)
{
    provider_config_t *hp = NULL;
    size_t hn = 0;
    if (put_file(path, kHwCaps) != 0)
        return 1;
    check_int("hw.rc", svc_load_model_config_yaml(path, &hp, &hn), 0);
    check_int("hw.count", (int)hn, 1);
    if (hp && hn == 1) {
        check_int("hw.declared", hp[0].hw.declared, 1);
        check_int("hw.accel", (int)hp[0].hw.accelerator, (int)AIRY_ACCEL_INTEL);
        check_int("hw.vram", (int)(hp[0].hw.vram_bytes >> 30), 8);
        check_int("hw.ctx", (int)hp[0].hw.max_context, 8192);
        check_int("hw.stream", hp[0].hw.supports_stream, 1);
        check_int("hw.tools", hp[0].hw.supports_tools, 0);
        free_providers(hp, hn);
    }

    provider_config_t *cp = NULL;
    size_t cn = 0;
    if (put_file(path, kFull8) != 0)
        return 1;
    check_int("cw.rc", svc_load_model_config_yaml(path, &cp, &cn), 0);
    check_int("cw.count", (int)cn, 1);
    if (cp && cn == 1) {
        check_int("cw.declared", cp[0].hw.declared, 1);
        check_int("cw.maxctx", (int)cp[0].hw.max_context, 8192);
        free_providers(cp, cn);
    }
    return 0;
}

/* 12)+13) 未声明走保守默认；非法/空值保持未知。 */
static int case_hw_undecl(const char *path)
{
    provider_config_t *np = NULL;
    size_t nn = 0;
    if (put_file(path, kFallback) != 0)
        return 1;
    check_int("hwundecl.rc", svc_load_model_config_yaml(path, &np, &nn), 0);
    if (np && nn == 1) {
        check_int("hwundecl.declared", np[0].hw.declared, 0);
        free_providers(np, nn);
    }

    provider_config_t *bp = NULL;
    size_t bn = 0;
    if (put_file(path, kHwBad) != 0)
        return 1;
    check_int("hwbad.rc", svc_load_model_config_yaml(path, &bp, &bn), 0);
    if (bp && bn == 1) {
        check_int("hwbad.declared", bp[0].hw.declared, 1);
        check_int("hwbad.accel", (int)bp[0].hw.accelerator, (int)AIRY_ACCEL_NONE);
        check_int("hwbad.vram", (int)bp[0].hw.vram_bytes, 0);
        check_int("hwbad.stream", bp[0].hw.supports_stream, -1);
        check_int("hwbad.tools", bp[0].hw.supports_tools, -1);
        free_providers(bp, bn);
    }
    return 0;
}

/* t11：采样覆盖落位与哨兵：未声明/非法值保持 -1，合法值原样贯通到
 *       provider 的每模型副表（声明点与消费点必须同源）。 */
static const char kSampling[] = "models:\n"
                                "  - name: s1\n"
                                "    model_id: s1\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n"
                                "    temperature: 0.15\n"
                                "    top_p: 0.8\n"
                                "  - name: s2\n"
                                "    model_id: s2\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n"
                                "  - name: s3\n"
                                "    model_id: s3\n"
                                "    provider: prov\n"
                                "    base_url: http://127.0.0.1:9/v1\n"
                                "    temperature: 5\n"
                                "    top_p: 0\n";

static int case_sampling(const char *path)
{
    svc_yaml_state_t st;
    memset(&st, 0, sizeof(st));
    if (put_file(path, kSampling) != 0)
        return 1;
    check_int("samp.rc", svc_yaml_load_state(path, &st), 0);
    check_int("samp.count", (int)st.model_count, 3);
    check_float("samp.m1.temp", st.models[0].temperature, 0.15f);
    check_float("samp.m1.top_p", st.models[0].top_p, 0.8f);
    check_float("samp.m2.temp", st.models[1].temperature, -1.0f);
    check_float("samp.m3.temp", st.models[2].temperature, -1.0f);
    check_float("samp.m3.top_p", st.models[2].top_p, -1.0f);

    provider_config_t *sp = NULL;
    size_t sn = 0;
    check_int("samp.agg.rc", svc_load_model_config_yaml(path, &sp, &sn), 0);
    check_int("samp.agg.count", (int)sn, 1);
    if (sp && sn == 1) {
        check_int("samp.agg.models", (int)(sp[0].models[0] != NULL), 1);
        check_float("samp.agg.temp", sp[0].model_temperature[0], 0.15f);
        check_float("samp.agg.top_p", sp[0].model_top_p[0], 0.8f);
        check_float("samp.agg.miss", sp[0].model_temperature[1], -1.0f);
        free_providers(sp, sn);
    }
    return 0;
}

int main(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/airy_llm_decl_%ld.yaml", (long)getpid());

    printf("running config declaration surface (A7-4) regression\n");

    if (case_caps(path) || case_unknown_key(path) || case_provision(path) ||
        case_missing(path) || case_full_rows(path) || case_hw_decl(path) ||
        case_hw_undecl(path) || case_sampling(path))
        return 1;

    remove(path);

    if (fail_count == 0) {
        printf("All %d decl assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d decl assertions failed\n", fail_count, test_count);
    return 1;
}
