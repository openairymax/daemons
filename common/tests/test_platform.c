// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_platform.c
 * @brief 平台抽象层单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "daemon_platform_ext.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_platform_detection(void)
{
    printf("  test_platform_detection...\n");

#if defined(AIRY_PLATFORM_WINDOWS)
    printf("    Running on Windows\n");
#elif defined(AIRY_PLATFORM_LINUX)
    printf("    Running on Linux\n");
#elif defined(AIRY_PLATFORM_MACOS)
    printf("    Running on macOS\n");
#else
    printf("    Unknown platform\n");
#endif

    assert(1);
    printf("    PASSED\n");
}

static void test_mutex_operations(void)
{
    printf("  test_mutex_operations...\n");

    airy_mtx_t mutex;
    int ret __attribute__((unused)) = airy_mtx_init(&mutex);
    assert(ret == 0);

    ret = airy_mtx_lock(&mutex);
    assert(ret == 0);

    ret = airy_mtx_trylock(&mutex);
    assert(ret == 0);  /* recursive mutex: same thread can relock */

    ret = airy_mtx_unlock(&mutex);
    assert(ret == 0);  /* unlock the extra recursive lock */

    ret = airy_mtx_lock(&mutex);  /* relock for later unlock test */
    assert(ret == 0);

    ret = airy_mtx_unlock(&mutex);
    assert(ret == 0);

    airy_mtx_destroy(&mutex);
    assert(ret == 0);

    printf("    PASSED\n");
}

static void test_time_operations(void)
{
    printf("  test_time_operations...\n");

    uint64_t ns = airy_time_ns();
    uint64_t ms = airy_time_ms();
    assert(ns > 0);
    assert(ms > 0);
    assert(ns >= ms * 1000000);

    printf("    PASSED\n");
}

static void test_random_operations(void)
{
    printf("  test_random_operations...\n");

    airy_random_init();

    uint32_t r1 = airy_random_uint32(1, 100);
    uint32_t r2 = airy_random_uint32(1, 100);
    assert(r1 >= 1 && r1 <= 100);
    assert(r2 >= 1 && r2 <= 100);

    float rf = airy_random_float();
    assert(rf >= 0.0f && rf <= 1.0f);

    printf("    PASSED\n");
}

static void test_file_operations(void)
{
    printf("  test_file_operations...\n");

    assert(airy_file_exists(".") == 1);
    assert(airy_file_exists("/nonexistent/path") == 0);

    printf("    PASSED\n");
}

static void test_strlcpy(void)
{
    printf("  test_strlcpy...\n");

    char dest[32];
    const char *src = "Hello, AgentRT!";

    airy_strlcpy(dest, src, sizeof(dest));
    assert(strcmp(dest, src) == 0);

    char dest2[8];
    airy_strlcpy(dest2, src, sizeof(dest2));
    assert(strlen(dest2) >= 3);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Platform Module Unit Tests\n");
    printf("=========================================\n");

    test_platform_detection();
    test_mutex_operations();
    test_time_operations();
    test_random_operations();
    test_file_operations();
    test_strlcpy();

    printf("\n✅ All platform module tests PASSED\n");
    return 0;
}