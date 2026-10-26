// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_envelope.c
 * @brief C11 请求信封参数段契约回归（provider_request_params_fill）
 *
 * 锁定机制面唯一装配点的三条不变量：
 *
 *   1. caps 能力位门控：位为假时显式请求也被丢弃（宁可不发不乱发）；
 *      位为真且值非零才下发——零值 = 未设置语义贯穿四可选字段
 *   2. response_format 仅在位真且为 LLM_RF_JSON_OBJECT 时下发嵌套对象
 *   3. temperature 哨兵三态：未显式设置时 0.0 等价缺省（0.7 兜底，
 *      旧请求体字节级不变）；显式设置后 0.0 必须原样下发
 *   4. T2 KV 复用透传：cache_prompt 随能力位恒发（位假即丢弃）；
 *      slot_id 仅显式设置（slot_set 哨兵）才下发，槽位 0 合法可表达
 */

#include "providers/core/provider_envelope.h"

#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

static void check_true(const char *name, int cond)
{
    test_count++;
    if (!cond) {
        printf("    FAIL: %s\n", name);
        fail_count++;
    }
}

static void check_absent(const cJSON *root, const char *key, const char *name)
{
    check_true(name, cJSON_GetObjectItem(root, key) == NULL);
}

static void caps_all(local_caps_t *caps, int on)
{
    memset(caps, 0, sizeof(*caps));
    caps->accepts_seed = on;
    caps->accepts_grammar = on;
    caps->accepts_response_format = on;
    caps->accepts_num_ctx = on;
}

static void fill_optional(llm_request_config_t *cfg)
{
    cfg->seed = 20260101u;
    cfg->grammar = "root ::= \"ok\"";
    cfg->response_format = LLM_RF_JSON_OBJECT;
    cfg->num_ctx = 8192u;
}

/* caps 位全假：显式请求也必须被丢弃，任何键不得漏出。 */
static void test_caps_gate_off(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;
    memset(&cfg, 0, sizeof(cfg));
    caps_all(&caps, 0);
    fill_optional(&cfg);
    cfg.caps = caps;

    cJSON *root = cJSON_CreateObject();
    check_true("gate_off.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, "def-model", 0, "stop");

    check_absent(root, "seed", "gate_off.seed withheld");
    check_absent(root, "grammar", "gate_off.grammar withheld");
    check_absent(root, "response_format", "gate_off.response_format withheld");
    check_absent(root, "num_ctx", "gate_off.num_ctx withheld");
    check_true("gate_off.model", cJSON_GetObjectItem(root, "model") != NULL);
    cJSON_Delete(root);
}

/* caps 位全真且值非零：四可选字段逐项落线，response_format 为嵌套对象。 */
static void test_caps_gate_on(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;
    memset(&cfg, 0, sizeof(cfg));
    caps_all(&caps, 1);
    fill_optional(&cfg);
    cfg.caps = caps;

    cJSON *root = cJSON_CreateObject();
    check_true("gate_on.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");

    cJSON *seed = cJSON_GetObjectItem(root, "seed");
    check_true("gate_on.seed", cJSON_IsNumber(seed) && seed->valuedouble == 20260101.0);

    cJSON *grammar = cJSON_GetObjectItem(root, "grammar");
    check_true("gate_on.grammar",
               cJSON_IsString(grammar) && strcmp(grammar->valuestring, "root ::= \"ok\"") == 0);

    cJSON *rf = cJSON_GetObjectItem(root, "response_format");
    cJSON *type = cJSON_GetObjectItem(rf, "type");
    check_true("gate_on.response_format",
               cJSON_IsString(type) && strcmp(type->valuestring, "json_object") == 0);

    cJSON *num_ctx = cJSON_GetObjectItem(root, "num_ctx");
    check_true("gate_on.num_ctx", cJSON_IsNumber(num_ctx) && num_ctx->valuedouble == 8192.0);
    cJSON_Delete(root);
}

/* 位真但值零：零值 = 未设置，不下发。 */
static void test_caps_on_zero_value(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;
    memset(&cfg, 0, sizeof(cfg));
    caps_all(&caps, 1);
    cfg.caps = caps;

    cJSON *root = cJSON_CreateObject();
    check_true("zero.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");

    check_absent(root, "seed", "zero.seed withheld");
    check_absent(root, "grammar", "zero.grammar withheld");
    check_absent(root, "response_format", "zero.response_format withheld");
    check_absent(root, "num_ctx", "zero.num_ctx withheld");
    cJSON_Delete(root);
}

/* temperature 哨兵三态：缺省 0.0 → 0.7 兜底；缺省正值 → 原值；显式 → 原值。 */
static void test_temperature_sentinel(void)
{
    llm_request_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.temperature = 0.0f;
    cJSON *root = cJSON_CreateObject();
    check_true("temp.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    cJSON *t = cJSON_GetObjectItem(root, "temperature");
    check_true("temp.default_zero_falls_back", cJSON_IsNumber(t) && t->valuedouble == 0.7);
    cJSON_Delete(root);

    memset(&cfg, 0, sizeof(cfg));
    cfg.temperature = 0.5f;
    root = cJSON_CreateObject();
    check_true("temp.root2", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    t = cJSON_GetObjectItem(root, "temperature");
    check_true("temp.legacy_positive", cJSON_IsNumber(t) && t->valuedouble == 0.5);
    cJSON_Delete(root);

    memset(&cfg, 0, sizeof(cfg));
    cfg.temperature = 0.0f;
    cfg.temperature_set = true;
    root = cJSON_CreateObject();
    check_true("temp.root3", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    t = cJSON_GetObjectItem(root, "temperature");
    check_true("temp.explicit_zero_sent", cJSON_IsNumber(t) && t->valuedouble == 0.0);
    cJSON_Delete(root);
}

/* T2 cache_prompt：位真恒发 true（策略在部署配置面表达，机制面忠实
 * 执行），slot_id 未显式设置时不得搭车漏出。 */
static void test_t2_cache_prompt(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;
    memset(&cfg, 0, sizeof(cfg));
    memset(&caps, 0, sizeof(caps));
    caps.accepts_cache_prompt = 1;
    cfg.caps = caps;

    cJSON *root = cJSON_CreateObject();
    check_true("t2cache.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    cJSON *cp = cJSON_GetObjectItem(root, "cache_prompt");
    check_true("t2cache.sent_when_cap", cJSON_IsBool(cp) && cJSON_IsTrue(cp));
    check_absent(root, "slot_id", "t2cache.slot_id withheld when unset");
    cJSON_Delete(root);
}

/* T2 位全关：KV 复用两项随白名单一起收口，任何键不得漏出。 */
static void test_t2_caps_off(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;
    memset(&cfg, 0, sizeof(cfg));
    caps_all(&caps, 0);
    fill_optional(&cfg);
    cfg.caps = caps;

    cJSON *root = cJSON_CreateObject();
    check_true("t2off.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    check_absent(root, "cache_prompt", "t2off.cache_prompt withheld");
    check_absent(root, "slot_id", "t2off.slot_id withheld");
    cJSON_Delete(root);
}

/* T2 slot_id 哨兵三态：未显式设置不下发（显式值也被丢弃）；显式设置
 * 原样下发，槽位 0 合法可表达。 */
static void test_t2_slot_id_sentinel(void)
{
    llm_request_config_t cfg;
    local_caps_t caps;

    memset(&cfg, 0, sizeof(cfg));
    memset(&caps, 0, sizeof(caps));
    caps.accepts_slot_id = 1;
    cfg.caps = caps;
    cfg.slot_id = 2;
    cJSON *root = cJSON_CreateObject();
    check_true("t2slot.root", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    check_absent(root, "slot_id", "t2slot.unset withheld despite value");
    cJSON_Delete(root);

    memset(&cfg, 0, sizeof(cfg));
    memset(&caps, 0, sizeof(caps));
    caps.accepts_slot_id = 1;
    cfg.caps = caps;
    cfg.slot_set = true;
    cfg.slot_id = 0;
    root = cJSON_CreateObject();
    check_true("t2slot.root2", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    cJSON *sid = cJSON_GetObjectItem(root, "slot_id");
    check_true("t2slot.zero_sent", cJSON_IsNumber(sid) && sid->valuedouble == 0.0);
    cJSON_Delete(root);

    memset(&cfg, 0, sizeof(cfg));
    memset(&caps, 0, sizeof(caps));
    caps.accepts_slot_id = 1;
    cfg.caps = caps;
    cfg.slot_set = true;
    cfg.slot_id = 2;
    root = cJSON_CreateObject();
    check_true("t2slot.root3", root != NULL);
    if (!root)
        return;
    provider_request_params_fill(root, &cfg, NULL, 0, "stop");
    sid = cJSON_GetObjectItem(root, "slot_id");
    check_true("t2slot.explicit_sent", cJSON_IsNumber(sid) && sid->valuedouble == 2.0);
    cJSON_Delete(root);
}

int main(void)
{
    printf("running C11 envelope params contract regression\n");
    test_caps_gate_off();
    test_caps_gate_on();
    test_caps_on_zero_value();
    test_temperature_sentinel();
    test_t2_cache_prompt();
    test_t2_caps_off();
    test_t2_slot_id_sentinel();

    if (fail_count == 0) {
        printf("All %d envelope assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d envelope assertions failed\n", fail_count, test_count);
    return 1;
}
