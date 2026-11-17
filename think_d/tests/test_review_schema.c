// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_review_schema.c
 * @brief C-4 双思考 B 侧黄金集：schema 校验 + 正文读取。
 *
 * 固定输入 → 期望结构 + 关键断言：
 *   1. 合法单行 JSON 的 drift/accept 与 reason 被严格取出
 *   2. 字段缺失 / 类型错 / 越界 / 空 reason / JSON 不完整一律判红
 *   3. t2/t1f 提示词必须包含 output_body 正文，且正文先于 output_json
 *   4. AIRY_REVIEW_SCHEMA 缺省 fail，只有 warn 才进入兼容档
 *
 * `--break` 模式只跑负例：故意破坏 schema 必须全部判红，供门禁取三值码。
 */

#include "review/review_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count;
static int fail_count;
static int neg_cases;

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
        printf("    FAIL: %s: missing \"%s\"\n", name, needle);
        fail_count++;
    }
}

static void expect_ok(const char *name, const char *content, const char *key,
                      int want_raw, const char *want_reason)
{
    int raw = -1;
    char reason[128] = "";
    int ok = rv_schema_parse(content, key, &raw, reason, sizeof(reason));
    check_int(name, ok, 1);
    if (ok) {
        check_int("raw", raw, want_raw);
        check_str("reason", reason, want_reason);
    }
}

static void expect_bad(const char *name, const char *content, const char *key)
{
    int raw = -1;
    char reason[128] = "";
    neg_cases++;
    check_int(name, rv_schema_parse(content, key, &raw, reason, sizeof(reason)), 0);
}

int main(int argc, char **argv)
{
    int break_mode = (argc > 1 && strcmp(argv[1], "--break") == 0);
    printf("running think.review schema golden set (C-4)\n");

    /* 1) 合法判决：含思考前缀、额外字段与空格变体。 */
    if (!break_mode) {
        expect_ok("ok.drift1", "{\"drift\":1,\"reason\":\"偏离目标\"}", "drift", 1, "偏离目标");
        expect_ok("ok.prefix", "思考：先核对正文\n{\"drift\": 0, \"reason\": \"满足目标\"}",
                  "drift", 0, "满足目标");
        expect_ok("ok.extra", "{\"drift\":0,\"reason\":\"ok\",\"extra\":1}", "drift", 0, "ok");
        expect_ok("ok.accept0", "{\"accept\":0,\"reason\":\"证据不足\"}", "accept", 0,
                  "证据不足");
        expect_ok("ok.accept1", "{\"accept\":1,\"reason\":\"通过\"}", "accept", 1, "通过");
        check_int("map.accept0", rv_accept_map(0), 1);
        check_int("map.accept1", rv_accept_map(1), 0);

        /* 2) 提示词：正文必须进 t2/t1f，且先于结构字段。 */
        char prompt[6144];
        const char *body = "产物正文：补丁已写入 src/a.c，并新增单测。";
        const char *json = "{\"files\":[\"src/a.c\"],\"status\":\"ok\"}";
        check_int("t2.prompt.rc", rv_prompt_t2(prompt, sizeof(prompt), "修复空指针", "tests",
                                             body, json), 0);
        check_has("t2.prompt.body", prompt, "output_body: 产物正文：补丁已写入");
        {
            const char *pb = strstr(prompt, "output_body");
            const char *pj = strstr(prompt, "output_json");
            check_int("t2.body.first", (pb && pj && pb < pj) ? 1 : 0, 1);
        }
        check_int("t1f.prompt.rc",
                  rv_prompt_t1f(prompt, sizeof(prompt), "gate ok", 1, "修复空指针", body, json),
                  0);
        check_has("t1f.prompt.body", prompt, body);
        check_has("t1f.prompt.gate", prompt, "gate_reason: gate ok");
        check_has("t1f.prompt.drift", prompt, "drift: 1");

        /* 3) 正文超长：截断标记必须出现，提示词不越界。 */
        char big[1600];
        size_t i = 0;
        for (; i + 1 < sizeof(big); ++i)
            big[i] = 'x';
        big[i] = '\0';
        check_int("t2.big.rc", rv_prompt_t2(prompt, sizeof(prompt), "g", "(无)", big, json), 0);
        check_has("t2.big.cut", prompt, "[正文已截断]");

        /* 4) 策略缺省 fail，仅 warn 进入兼容档。 */
        unsetenv("AIRY_REVIEW_SCHEMA");
        check_int("mode.default", rv_schema_mode(), 0);
        setenv("AIRY_REVIEW_SCHEMA", "warn", 1);
        check_int("mode.warn", rv_schema_mode(), 1);
        setenv("AIRY_REVIEW_SCHEMA", "fail", 1);
        check_int("mode.fail", rv_schema_mode(), 0);
        unsetenv("AIRY_REVIEW_SCHEMA");
    }

    /* 5) 负例：故意破坏 schema 必须判红。 */
    expect_bad("bad.nokey", "{\"reason\":\"x\"}", "drift");
    expect_bad("bad.noreason", "{\"drift\":1}", "drift");
    expect_bad("bad.emptyreason", "{\"drift\":1,\"reason\":\"\"}", "drift");
    expect_bad("bad.strval", "{\"drift\":\"1\",\"reason\":\"x\"}", "drift");
    expect_bad("bad.range", "{\"drift\":2,\"reason\":\"x\"}", "drift");
    expect_bad("bad.reasonnum", "{\"drift\":1,\"reason\":123}", "drift");
    expect_bad("bad.unterminated", "{\"drift\":0,\"reason\":\"ok\"", "drift");
    expect_bad("bad.plain", "no json at all", "drift");
    expect_bad("bad.accept2", "{\"accept\":2,\"reason\":\"x\"}", "accept");
    {
        char longreason[512];
        size_t o = (size_t)snprintf(longreason, sizeof(longreason), "{\"drift\":1,\"reason\":\"");
        for (int k = 0; k < 201; ++k)
            longreason[o++] = 'a';
        snprintf(longreason + o, sizeof(longreason) - o, "\"}");
        expect_bad("bad.longreason", longreason, "drift");
    }

    if (break_mode) {
        printf("BREAK: %d broken-schema fixtures rejected\n", neg_cases);
        return fail_count ? 1 : 0;
    }

    printf("REVIEW-SCHEMA RESULT: %d checks, %d failures\n", test_count, fail_count);
    return fail_count ? 1 : 0;
}
