// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_secrets_perm.c
 * @brief 0.1.21 S30-P1-8：secrets.env 权限收紧与取值优先级回归。
 *
 * 覆盖读前收紧（0644 → 0600）、收紧后值可读、进程环境覆盖文件值，
 * 以及缺少变量时不再误用文件内容。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airy_prov/secrets.h"

static int g_fail;

#define EXPECT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

#define TEST_KEY_ENV "AIRY_TEST_PROV_KEY"

static int secrets_fixture(char *dir, size_t dir_size, char *path, size_t path_size)
{
    char tmpl[] = "/tmp/airy_secrets_XXXXXX";
    if (!mkdtemp(tmpl))
        return 0;
    snprintf(dir, dir_size, "%s", tmpl);

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config", dir);
    if (mkdir(cfg, 0700) != 0 && access(cfg, F_OK) != 0)
        return 0;

    snprintf(path, path_size, "%s/secrets.env", cfg);
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fputs(TEST_KEY_ENV "=sk-from-file\n", f);
    fclose(f);
    return chmod(path, 0644) == 0;
}

int main(void)
{
    char dir[512];
    char path[600];
    EXPECT(secrets_fixture(dir, sizeof(dir), path, sizeof(path)));
    if (g_fail)
        return 1;

    setenv("AIRY_HOME", dir, 1);
    unsetenv(TEST_KEY_ENV);

    provider_base_ctx_t base;
    memset(&base, 0, sizeof(base));
    snprintf(base.api_key_env, sizeof(base.api_key_env), "%s", TEST_KEY_ENV);

    provider_refresh_api_key(&base);
    EXPECT(strcmp(base.api_key, "sk-from-file") == 0);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 077) == 0);

    /* 进程环境优先于文件：改 env 立即生效，文件内容不再参与。 */
    setenv(TEST_KEY_ENV, "sk-from-env", 1);
    provider_refresh_api_key(&base);
    EXPECT(strcmp(base.api_key, "sk-from-env") == 0);

    /* 变量名指向的键不存在时，不残留上一次的值。 */
    provider_base_ctx_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    snprintf(fresh.api_key_env, sizeof(fresh.api_key_env), "%s", "AIRY_TEST_ABSENT_KEY");
    provider_refresh_api_key(&fresh);
    EXPECT(fresh.api_key[0] == '\0');

    unlink(path);
    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config", dir);
    rmdir(cfg);
    rmdir(dir);

    printf(g_fail ? "FAIL %d\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
#else
int main(void)
{
    printf("SKIP: POSIX 模式位在 Windows 不适用（凭据目录由用户 ACL 隔离）\n");
    return 0;
}
#endif
