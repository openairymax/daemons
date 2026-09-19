// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_stats_json.c
 * @brief mem.get_stats 统计构造单元测试。
 *
 * 覆盖：语义缓存命中率、上下文台账统计在 mem.get_stats 中的嵌套输出，
 * 以及 mem.cache_stats / mem.ledger_stats 与 mem.get_stats 的口径一致性
 * （13-semantic-cache-context-ledger.md §6 测试要点）。
 */

#include "mem_handlers.h"
#include "mem_daemon_ctx.h"

#include "cache.h"
#include "cache_handlers.h"
#include "ledger.h"
#include "ledger_handlers.h"

#include "airy_memory.h"
#include "error.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

mem_service_t *g_service = NULL;
mem_cache_t *g_cache = NULL;
mem_ledger_t *g_ledger = NULL;
mem_daemon_config_t g_config = {0};

/* 缓存未就绪时不得构造子对象，避免用户面出现半截统计。 */
static void test_uninit_degrade(void)
{
    printf("  test_uninit_degrade...\n");
    assert(mem_cache_stats_json() == NULL);
    assert(mem_ledger_stats_json() == NULL);

    cJSON *root = mem_stats_json();
    assert(root != NULL);
    assert(cJSON_GetObjectItem(root, "cache") == NULL);
    assert(cJSON_GetObjectItem(root, "ledger") == NULL);
    cJSON_Delete(root);
    printf("    PASSED\n");
}

/* B5-3（V5.2）：敏感面请求或未声明 cacheable 的请求不得产生缓存条目。 */
static void test_cache_put_admission(void)
{
    printf("  test_cache_put_admission...\n");
    g_cache = mem_cache_create(64, 0, 0, 0.85);
    assert(g_cache != NULL);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    mem_cache_stats_t st;

    /* 含敏感标记（私有路径 + 凭据）⇒ 条目数为 0 */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "text", "read ~/.ssh/id_rsa with api_key=abc");
    cJSON_AddStringToObject(params, "response", "{\"answer\":\"x\"}");
    cJSON_AddStringToObject(params, "model_id", "m1");
    cJSON_AddBoolToObject(params, "cacheable", 1);
    handle_cache_put(params, 1, fds[0]);
    cJSON_Delete(params);

    mem_cache_stats(g_cache, &st);
    assert(st.entries == 0);

    /* 普通文本但未显式声明 cacheable ⇒ 仍不入缓存（fail-closed） */
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "text", "alpha query");
    cJSON_AddStringToObject(params, "response", "alpha resp");
    cJSON_AddStringToObject(params, "model_id", "m1");
    handle_cache_put(params, 2, fds[0]);
    cJSON_Delete(params);

    mem_cache_stats(g_cache, &st);
    assert(st.entries == 0);

    /* 显式声明 cacheable 的普通文本 ⇒ 正常入缓存 */
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "text", "alpha query");
    cJSON_AddStringToObject(params, "response", "alpha resp");
    cJSON_AddStringToObject(params, "model_id", "m1");
    cJSON_AddBoolToObject(params, "cacheable", 1);
    handle_cache_put(params, 3, fds[0]);
    cJSON_Delete(params);

    mem_cache_stats(g_cache, &st);
    assert(st.entries == 1);

    close(fds[0]);
    close(fds[1]);
    mem_cache_destroy(g_cache);
    g_cache = NULL;
    printf("    PASSED\n");
}

/* mem.get_stats 的 cache 子对象须与 mem.cache_stats 口径逐字段一致。 */
static void test_cache_scope_consistent(void)
{
    printf("  test_cache_scope_consistent...\n");
    g_cache = mem_cache_create(64, 0, 0, 0.85);
    assert(g_cache != NULL);

    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(g_cache, "alpha query", "alpha resp", "m1", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    int hit = 0;
    char *out = NULL;
    assert(mem_cache_get(g_cache, "alpha query", "m1", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);

    hit = 1;
    assert(mem_cache_get(g_cache, "unrelated query text", "m1", 0, &hit, NULL, NULL, &out) ==
           AIRY_SUCCESS);
    assert(hit == 0);

    mem_cache_stats_t st;
    mem_cache_stats(g_cache, &st);
    assert(st.hits == 1 && st.misses == 1);
    assert(fabs(st.hit_rate - 0.5) < 1e-9);

    cJSON *root = mem_stats_json();
    assert(root != NULL);
    cJSON *cache = cJSON_GetObjectItem(root, "cache");
    assert(cJSON_IsObject(cache));
    assert(cJSON_GetObjectItem(cache, "entries")->valueint == 1);
    assert(cJSON_GetObjectItem(cache, "hits")->valueint == 1);
    assert(cJSON_GetObjectItem(cache, "misses")->valueint == 1);
    assert(fabs(cJSON_GetObjectItem(cache, "hit_rate")->valuedouble - st.hit_rate) < 1e-9);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cache, "hit_rate_available")));
    assert(cJSON_GetObjectItem(cache, "evictions")->valueint == 0);
    assert(cJSON_GetObjectItem(cache, "bytes")->valueint == (int)st.bytes);

    cJSON *standalone = mem_cache_stats_json();
    assert(cJSON_IsObject(standalone));
    assert(cJSON_GetObjectItem(standalone, "hit_rate")->valuedouble ==
           cJSON_GetObjectItem(cache, "hit_rate")->valuedouble);
    cJSON_Delete(standalone);

    /* 无查询样本 → hit_rate 不可用（负值 + available=false），不得伪零（§2.1-6） */
    mem_cache_t *fresh = mem_cache_create(8, 0, 0, 0.85);
    assert(fresh != NULL);
    mem_cache_t *saved = g_cache;
    g_cache = fresh;
    cJSON *unavail = mem_cache_stats_json();
    assert(cJSON_IsObject(unavail));
    assert(cJSON_GetObjectItem(unavail, "hit_rate")->valuedouble < 0.0);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(unavail, "hit_rate_available")));
    cJSON_Delete(unavail);
    g_cache = saved;
    mem_cache_destroy(fresh);

    cJSON_Delete(root);
    mem_cache_destroy(g_cache);
    g_cache = NULL;
    printf("    PASSED\n");
}

/* mem.get_stats 的 ledger 子对象须与 mem.ledger_stats 口径逐字段一致。 */
static void test_ledger_scope_consistent(void)
{
    printf("  test_ledger_scope_consistent...\n");
    g_ledger = mem_ledger_create(0, 0);
    assert(g_ledger != NULL);

    ledger_entry_in_t in = {
        .entry_type = LEDGER_ENTRY_USER,
        .text = "a user message long enough to yield tokens",
        .token_in = 12,
        .token_out = 0,
        .source = "gateway",
        .ref_id = NULL,
    };
    char *batch_id = NULL;
    assert(mem_ledger_append(g_ledger, "sess-1", &in, 1, &batch_id) == AIRY_SUCCESS);
    AIRY_FREE(batch_id);

    mem_ledger_stats_t st;
    mem_ledger_stats(g_ledger, &st);
    assert(st.sessions == 1 && st.entries == 1 && st.total_tokens == 12);

    cJSON *root = mem_stats_json();
    assert(root != NULL);
    cJSON *ledger = cJSON_GetObjectItem(root, "ledger");
    assert(cJSON_IsObject(ledger));
    assert(cJSON_GetObjectItem(ledger, "sessions")->valueint == 1);
    assert(cJSON_GetObjectItem(ledger, "entries")->valueint == 1);
    assert(cJSON_GetObjectItem(ledger, "total_tokens")->valueint == 12);

    cJSON *standalone = mem_ledger_stats_json();
    assert(cJSON_IsObject(standalone));
    assert(cJSON_GetObjectItem(standalone, "total_tokens")->valueint ==
           cJSON_GetObjectItem(ledger, "total_tokens")->valueint);
    cJSON_Delete(standalone);

    cJSON_Delete(root);
    mem_ledger_destroy(g_ledger);
    g_ledger = NULL;
    printf("    PASSED\n");
}

/* 顶层基础字段保持原有语义（records/max_records/daemon）。 */
static void test_base_fields(void)
{
    printf("  test_base_fields...\n");
    g_config.max_records = 512;

    cJSON *root = mem_stats_json();
    assert(root != NULL);
    cJSON *daemon = cJSON_GetObjectItem(root, "daemon");
    assert(cJSON_IsString(daemon) && strcmp(daemon->valuestring, "mem_d") == 0);
    assert(cJSON_GetObjectItem(root, "records")->valueint == 0);
    assert(cJSON_GetObjectItem(root, "max_records")->valueint == 512);

    cJSON_Delete(root);
    printf("    PASSED\n");
}

/* 读取 socketpair 上的 JSON-RPC 响应（SOCK_STREAM 可能分片，读到可解析为止）。 */
static cJSON *read_rpc_response(int fd)
{
    size_t cap = 4096, len = 0;
    char *buf = AIRY_MALLOC(cap);
    assert(buf != NULL);
    cJSON *root = NULL;
    while (len + 1 < cap) {
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0)
            break;
        len += (size_t)r;
        buf[len] = '\0';
        root = cJSON_Parse(buf);
        if (root)
            break;
    }
    AIRY_FREE(buf);
    assert(root != NULL);
    return root;
}

/* B5/V5.1 + V5.3：mem.compress 门禁快照。
 * 默认声明（L2 关 + 门禁全关）→ fail-closed：allowed=false、acr/ttft 不可用（负值）
 * 且 L2 不生效；声明面注入实测值（灰度开 + acr/ttft 达标）→ allowed=true、
 * acr/ttft 落数值记录（非桩值）且 L2 生效。 */
static void test_compress_gate_snapshot(void)
{
    printf("  test_compress_gate_snapshot...\n");
    g_ledger = mem_ledger_create(20, 0.8); /* 小预算：使 L2 具备触发条件 */
    assert(g_ledger != NULL);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    const char *long_msg =
        "First we need to analyze the requirement and understand the constraints. "
        "The system must ensure data safety and verify every step carefully. "
        "We should document the design decisions and confirm the final result. "
        "Please review the implementation and make sure nothing is missing. "
        "The summary should include the conclusion and the open questions.";

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "session_id", "sess-gate");
    cJSON *entries = cJSON_CreateArray();
    const char *eids[] = {"g1", "g2", "g3", "g4"};
    const char *etypes[] = {"system", "user", "assistant", "user"};
    const char *etexts[] = {"system prompt", "first user message", long_msg, "continue please"};
    for (int i = 0; i < 4; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "entry_id", eids[i]);
        cJSON_AddStringToObject(e, "entry_type", etypes[i]);
        cJSON_AddStringToObject(e, "text", etexts[i]);
        cJSON_AddItemToArray(entries, e);
    }
    cJSON_AddItemToObject(params, "entries", entries);

    /* V5.1：默认声明 → fail-closed；acr/ttft 呈现"不可用"（负值）而非伪零 */
    g_config.compress_l1_enabled = 1;
    g_config.compress_l2_enabled = 0;
    g_config.compress_gate_grayscale = 0;
    g_config.compress_gate_acr = -1.0;
    g_config.compress_gate_ttft_ms = -1.0;

    handle_compress(params, 1, fds[0]);
    cJSON *resp = read_rpc_response(fds[1]);
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsObject(result));
    cJSON *gate = cJSON_GetObjectItem(result, "gate");
    assert(cJSON_IsObject(gate));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(gate, "allowed")));
    assert(cJSON_GetObjectItem(gate, "acr")->valuedouble < 0.0);
    assert(cJSON_GetObjectItem(gate, "ttft_ms")->valuedouble < 0.0);
    assert(cJSON_GetObjectItem(result, "saved_tokens")->valuedouble == 0.0);
    cJSON_Delete(resp);

    /* V5.3：门禁翻转（灰度开 + acr/ttft 达标）→ L2 生效且数值记录非桩值 */
    g_config.compress_l2_enabled = 1;
    g_config.compress_gate_grayscale = 1;
    g_config.compress_gate_acr = 0.99;
    g_config.compress_gate_ttft_ms = 100.0;

    handle_compress(params, 2, fds[0]);
    resp = read_rpc_response(fds[1]);
    result = cJSON_GetObjectItem(resp, "result");
    assert(cJSON_IsObject(result));
    gate = cJSON_GetObjectItem(result, "gate");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(gate, "allowed")));
    assert(fabs(cJSON_GetObjectItem(gate, "acr")->valuedouble - 0.99) < 1e-9);
    assert(fabs(cJSON_GetObjectItem(gate, "ttft_ms")->valuedouble - 100.0) < 1e-9);
    assert(cJSON_GetObjectItem(result, "saved_tokens")->valuedouble > 0.0);
    cJSON_Delete(resp);

    cJSON_Delete(params);
    close(fds[0]);
    close(fds[1]);
    mem_ledger_destroy(g_ledger);
    g_ledger = NULL;
    g_config.compress_l2_enabled = 0;
    g_config.compress_gate_grayscale = 0;
    g_config.compress_gate_acr = -1.0;
    g_config.compress_gate_ttft_ms = -1.0;
    printf("    PASSED\n");
}

int main(void)
{
    printf("=== mem.get_stats JSON Unit Tests ===\n");
    test_uninit_degrade();
    test_cache_put_admission();
    test_cache_scope_consistent();
    test_ledger_scope_consistent();
    test_base_fields();
    test_compress_gate_snapshot();
    printf("=== All stats JSON tests PASSED ===\n");
    return 0;
}
