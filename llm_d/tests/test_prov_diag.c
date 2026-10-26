// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_prov_diag.c
 * @brief 0.1.20 B1-d：上游失败线程局域诊断槽回归——写读、清空、截断、
 *        后写覆盖、纯状态码、传输层形态与线程隔离。
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "platform_base.h"
#include "providers/core/prov_diag.h"

static int g_fail;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static void test_set_and_fetch(void)
{
    prov_diag_clear();
    EXPECT(prov_diag_text()[0] == '\0');

    const char *body = "{\"error\":\"tools[13] invalid\"}";
    prov_diag_set(400, body, strlen(body));
    const char *t = prov_diag_text();
    EXPECT(strstr(t, "upstream http 400:") != NULL);
    EXPECT(strstr(t, "tools[13] invalid") != NULL);
}

static void test_truncate(void)
{
    char body[PROV_DIAG_BODY_MAX + 256];
    memset(body, 'A', sizeof(body));
    prov_diag_set(503, body, sizeof(body));

    const char *t = prov_diag_text();
    size_t prefix = strlen("upstream http 503: ");
    EXPECT(strlen(t) == prefix + PROV_DIAG_BODY_MAX);
    EXPECT(strncmp(t, "upstream http 503: ", prefix) == 0);
    EXPECT(t[prefix] == 'A' && t[strlen(t) - 1] == 'A');
}

static void test_overwrite(void)
{
    prov_diag_set(429, "first-attempt", 13);
    prov_diag_set(500, "last-attempt", 12);
    const char *t = prov_diag_text();
    EXPECT(strstr(t, "upstream http 500:") != NULL);
    EXPECT(strstr(t, "last-attempt") != NULL);
    EXPECT(strstr(t, "first-attempt") == NULL);
}

static void test_code_only_and_transport(void)
{
    prov_diag_set(401, NULL, 0);
    EXPECT(strcmp(prov_diag_text(), "upstream http 401 (empty body)") == 0);

    prov_diag_set(0, "dns failure", 11);
    EXPECT(strcmp(prov_diag_text(), "upstream transport: dns failure") == 0);

    /* code<=0 且无 body：等价 clear，不得留陈旧诊断。 */
    prov_diag_set(0, NULL, 0);
    EXPECT(prov_diag_text()[0] == '\0');
}

static void *thr_probe(void *arg)
{
    const char *snapshot = (const char *)arg;
    strncpy((char *)snapshot, prov_diag_text(), 63);
    prov_diag_set(502, "from-thread", 11);
    return NULL;
}

static void test_thread_isolation(void)
{
    char snapshot[64];
    snapshot[0] = '\0';

    prov_diag_set(404, "main-thread-body", 16);
    pthread_t th;
    EXPECT(pthread_create(&th, NULL, thr_probe, snapshot) == 0);
    pthread_join(th, NULL);

    /* 子线程首读须为空（TLS 隔离），且不得改写主线程槽。 */
    EXPECT(snapshot[0] == '\0');
    EXPECT(strstr(prov_diag_text(), "main-thread-body") != NULL);
}

int main(void)
{
    test_set_and_fetch();
    test_truncate();
    test_overwrite();
    test_code_only_and_transport();
    test_thread_isolation();

    if (g_fail != 0) {
        printf("test_prov_diag: %d assertion(s) failed\n", g_fail);
        return 1;
    }
    printf("test_prov_diag: all assertions passed\n");
    return 0;
}
