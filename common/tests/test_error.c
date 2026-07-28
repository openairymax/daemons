// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_error.c
 * @brief 错误处理模块单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "airy_types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

static void test_error_strerror(void)
{
    printf("  test_error_strerror...\n");

    assert(airy_strerror(AIRY_SUCCESS) != NULL);
    assert(airy_strerror(AIRY_EUNKNOWN) != NULL);
    assert(airy_strerror(AIRY_EINVAL) != NULL);
    assert(airy_strerror(AIRY_ENOMEM) != NULL);
    assert(airy_strerror(AIRY_ENOENT) != NULL);
    assert(airy_strerror(AIRY_ETIMEDOUT) != NULL);
    assert(airy_strerror(AIRY_EIO) != NULL);
    assert(airy_strerror(-999) != NULL);

    printf("    PASSED\n");
}

static void test_error_codes(void)
{
    printf("  test_error_codes...\n");

    assert(AIRY_SUCCESS == 0);
    assert(AIRY_EUNKNOWN != 0);
    assert(AIRY_EINVAL != 0);
    assert(AIRY_ENOMEM != 0);

    printf("    PASSED\n");
}

static void test_error_new_codes(void)
{
    printf("  test_error_new_codes...\n");

    /* v3.0 SSoT 统一收敛：与 POSIX errno 负值冲突的 AIRY_ERR_* 扩展码
     * 已迁移至 -36~-60 区间（第三轮迁移，避免与 IPC 码空间冲突）。
     * 详见 error.h 注释（原 -2→-40→-36 等迁移轨迹）。 */
    assert(AIRY_ERR_INVALID_PARAM == -36);
    assert(AIRY_ERR_NULL_POINTER == -3);
    assert(AIRY_ERR_OUT_OF_MEMORY == -59);
    assert(AIRY_ERR_NOT_FOUND == -6);
    assert(AIRY_ERR_ALREADY_EXISTS == -38);
    assert(AIRY_ERR_TIMEOUT == -8);
    assert(AIRY_ERR_NOT_SUPPORTED == -9);
    assert(AIRY_ERR_PERMISSION_DENIED == -39);
    assert(AIRY_ERR_IO == -40);
    assert(AIRY_ERR_OVERFLOW == -60);
    assert(AIRY_ERR_CANCELED == -57);
    assert(AIRY_ERR_BUSY == -58);
    assert(AIRY_ERR_INTERRUPTED == -19);

    assert(AIRY_ERR_SYS_NOT_INIT == -101);
    assert(AIRY_ERR_SYS_RESOURCE == -102);

    assert(airy_strerror(AIRY_ERR_INVALID_PARAM) != NULL);
    assert(airy_strerror(AIRY_ERR_OUT_OF_MEMORY) != NULL);
    assert(airy_strerror(AIRY_ERR_NOT_FOUND) != NULL);
    assert(airy_strerror(AIRY_ERR_OVERFLOW) != NULL);
    assert(airy_strerror(AIRY_ERR_SYS_NOT_INIT) != NULL);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Error Module Unit Tests\n");
    printf("=========================================\n");

    test_error_strerror();
    test_error_codes();
    test_error_new_codes();

    printf("\nAll error module tests PASSED\n");
    return 0;
}
