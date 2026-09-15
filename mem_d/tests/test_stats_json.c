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
    assert(cJSON_GetObjectItem(cache, "evictions")->valueint == 0);
    assert(cJSON_GetObjectItem(cache, "bytes")->valueint == (int)st.bytes);

    cJSON *standalone = mem_cache_stats_json();
    assert(cJSON_IsObject(standalone));
    assert(cJSON_GetObjectItem(standalone, "hit_rate")->valuedouble ==
           cJSON_GetObjectItem(cache, "hit_rate")->valuedouble);
    cJSON_Delete(standalone);

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

int main(void)
{
    printf("=== mem.get_stats JSON Unit Tests ===\n");
    test_uninit_degrade();
    test_cache_scope_consistent();
    test_ledger_scope_consistent();
    test_base_fields();
    printf("=== All stats JSON tests PASSED ===\n");
    return 0;
}
