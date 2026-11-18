// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_verify.c
 * @brief C-17 完成判据复核器单元测试（四类判据 × 正负例 + 入口防御）。
 *
 * 复核器只读取证、不做状态裁决：本测试断言的是「取证是否如实」——
 * 通过/不通过、逐项 expect/actual、以及非法输入一律 fail-closed。
 */

#include "verify/tool_verify.h"

#include "airy_verify.h"

#include <cjson/cJSON.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CASE_FILE "tool_verify_case.txt"

static cJSON *verify(const char *json, int timeout_ms)
{
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    cJSON *res = tool_verify_run(root, timeout_ms);
    cJSON_Delete(root);
    assert(res != NULL);
    return res;
}

static int ok_of(const cJSON *res)
{
    return cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)res, AIRY_VK_OK));
}

static cJSON *item0(const cJSON *res)
{
    cJSON *items = cJSON_GetObjectItem((cJSON *)res, AIRY_VK_ITEMS);
    return cJSON_GetArrayItem(items, 0);
}

static const char *field(const cJSON *res, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(item0(res), key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int num_of(const cJSON *res, const char *key)
{
    return cJSON_GetObjectItem((cJSON *)res, key)->valueint;
}

static void case_reject_empty(void)
{
    printf("  test_reject_empty...\n");
    cJSON *res = verify("[]", 1000);
    assert(!ok_of(res));
    assert(num_of(res, AIRY_VK_TOTAL) == 0);
    printf("    PASSED\n");
    cJSON_Delete(res);
}

static void case_reject_shape(void)
{
    printf("  test_reject_shape...\n");
    cJSON *res = verify("{\"criteria\":[]}", 1000);
    assert(!ok_of(res));
    cJSON_Delete(res);

    res = verify("\"not-an-array\"", 1000);
    assert(!ok_of(res));
    cJSON_Delete(res);
}

static void case_reject_too_many(void)
{
    printf("  test_reject_too_many...\n");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i <= AIRY_VERIFY_MAX_ITEMS; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, AIRY_VC_TYPE, AIRY_VC_T_FILE);
        cJSON_AddStringToObject(it, AIRY_VC_PATH, CASE_FILE);
        cJSON_AddItemToArray(arr, it);
    }
    cJSON *res = tool_verify_run(arr, 1000);
    assert(res && !ok_of(res));
    printf("    PASSED\n");
    cJSON_Delete(res);
    cJSON_Delete(arr);
}

static void case_unknown_type(void)
{
    printf("  test_unknown_type...\n");
    cJSON *res = verify("[{\"type\":\"bogus\"}]", 1000);
    assert(!ok_of(res));
    assert(strcmp(field(res, AIRY_VK_ACTUAL), "") != 0);
    printf("    PASSED\n");
    cJSON_Delete(res);
}

static void case_file(void)
{
    printf("  test_file...\n");
    FILE *f = fopen(CASE_FILE, "wb");
    assert(f != NULL);
    fputs("build ok\nALL-DONE\n", f);
    fclose(f);

    cJSON *res = verify("[{\"type\":\"file\",\"path\":\"" CASE_FILE "\",\"contains\":\"ALL-"
                        "DONE\"}]",
                        1000);
    assert(ok_of(res));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(item0(res), AIRY_VK_PASS)));
    cJSON_Delete(res);

    res = verify("[{\"type\":\"file\",\"path\":\"" CASE_FILE "\",\"contains\":\"MISSING\"}]",
                 1000);
    assert(!ok_of(res));
    assert(strstr(field(res, AIRY_VK_ACTUAL), "match=no") != NULL);
    cJSON_Delete(res);

    res = verify("[{\"type\":\"file\",\"path\":\"no_such_artifact_xyz\"}]", 1000);
    assert(!ok_of(res));
    assert(strstr(field(res, AIRY_VK_ACTUAL), "missing") != NULL);
    cJSON_Delete(res);
    remove(CASE_FILE);
    printf("    PASSED\n");
}

#ifndef _WIN32
static void case_cmd(void)
{
    printf("  test_cmd...\n");
    cJSON *res = verify("[{\"type\":\"cmd\",\"cmd\":\"true\"}]", 5000);
    assert(ok_of(res));
    cJSON_Delete(res);

    res = verify("[{\"type\":\"cmd\",\"cmd\":\"false\"}]", 5000);
    assert(!ok_of(res));
    assert(strstr(field(res, AIRY_VK_ACTUAL), "exit_code=1") != NULL);
    cJSON_Delete(res);
    printf("    PASSED\n");
}

static void case_service(void)
{
    printf("  test_service...\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 4) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &alen) == 0);
    int port = ntohs(addr.sin_port);

    char json[160];
    snprintf(json, sizeof(json), "[{\"type\":\"service\",\"port\":%d}]", port);
    cJSON *res = verify(json, 2000);
    assert(ok_of(res));
    cJSON_Delete(res);
    close(fd);

    res = verify(json, 500);
    assert(!ok_of(res));
    assert(strcmp(field(res, AIRY_VK_ACTUAL), "unreachable") == 0);
    cJSON_Delete(res);
    printf("    PASSED\n");
}
#endif

static void case_rpc(void)
{
    printf("  test_rpc...\n");
    /* 写型/未登记方法一律在只读白名单外，必须 fail-closed。 */
    cJSON *res = verify("[{\"type\":\"rpc\",\"daemon\":\"tool\",\"method\":\"tool_execute\"}]",
                        2000);
    assert(!ok_of(res));
    assert(strstr(field(res, AIRY_VK_ACTUAL), "allowlist") != NULL);
    cJSON_Delete(res);

    /* 只读方法但守护进程不可达：取证失败，不得判通过。 */
    res = verify("[{\"type\":\"rpc\",\"daemon\":\"nosuchd\",\"method\":\"list\"}]", 500);
    assert(!ok_of(res));
    cJSON_Delete(res);
    printf("    PASSED\n");
}

static void case_mixed(void)
{
    printf("  test_mixed...\n");
    FILE *f = fopen(CASE_FILE, "wb");
    assert(f != NULL);
    fputs("DONE\n", f);
    fclose(f);
    cJSON *res = verify("[{\"type\":\"file\",\"path\":\"" CASE_FILE "\"},"
                        "{\"type\":\"file\",\"path\":\"no_such_artifact_xyz\"}]",
                        1000);
    assert(!ok_of(res));
    assert(num_of(res, AIRY_VK_TOTAL) == 2);
    assert(num_of(res, AIRY_VK_PASSED) == 1);
    printf("    PASSED\n");
    cJSON_Delete(res);
    remove(CASE_FILE);
}

int main(void)
{
    printf("=== tool_verify (C-17) ===\n");
    case_reject_empty();
    case_reject_shape();
    case_reject_too_many();
    case_unknown_type();
    case_file();
#ifndef _WIN32
    case_cmd();
    case_service();
#endif
    case_rpc();
    case_mixed();
    printf("=== ALL PASSED ===\n");
    return 0;
}
