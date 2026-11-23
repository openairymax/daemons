// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_plugin_discovery.c
 * @brief 插件发现容量自检：满额时只收前 PLUGIN_DISCOVERY_MAX_PLUGINS 个
 *        有效插件，并如实计数被容量挡下的目录；无 manifest 的目录既不计入
 *        结果，也不计入容量外条目（否则告警会虚报）。
 */

#include "plugin/plugin_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#ifndef _WIN32

static char g_root[256];

static int mkdir_p(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* 造一个有效插件目录：name 必填，api_version/min_airy_version 非零才过校验 */
static int make_plugin(int idx)
{
    char dir[512], yml[600];
    snprintf(dir, sizeof(dir), "%s/plugins/p%03d", g_root, idx);
    if (mkdir_p(dir) != 0)
        return -1;
    snprintf(yml, sizeof(yml), "%s/manifest.yaml", dir);
    FILE *fp = fopen(yml, "w");
    if (!fp)
        return -1;
    fprintf(fp, "name: plug%03d\nversion: 1.0.0\ntype: tool_provider\n"
                "api_version: 1\nmin_airy_version: 1\n",
            idx);
    fclose(fp);
    return 0;
}

static void test_capacity(void)
{
    printf("  test_capacity...\n");
    snprintf(g_root, sizeof(g_root), "/tmp/disco_cap_%d", (int)getpid());
    char plugins[512], cmd[600];
    snprintf(plugins, sizeof(plugins), "%s/plugins", g_root);
    CHECK(mkdir_p(g_root) == 0);
    CHECK(mkdir_p(plugins) == 0);

    /* 130 个候选（容量 128 之外多两枚），外加两个无 manifest 的目录 */
    for (int i = 0; i < PLUGIN_DISCOVERY_MAX_PLUGINS + 2; i++)
        CHECK(make_plugin(i) == 0);
    char noise[512];
    snprintf(noise, sizeof(noise), "%s/plugins/not_a_plugin", g_root);
    CHECK(mkdir_p(noise) == 0);

    plugin_discovery_config_t cfg = {plugins, false};
    CHECK(plugin_discovery_init(&cfg) == 0);

    plugin_discovery_result_t *results = NULL;
    size_t count = 0;
    CHECK(plugin_discovery_scan(&results, &count) == 0);
    CHECK(count == PLUGIN_DISCOVERY_MAX_PLUGINS);
    for (size_t i = 0; i < count; i++)
        CHECK(results && results[i].valid && results[i].name[0] != '\0');
    plugin_discovery_free_results(results, count);
    plugin_discovery_destroy();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    CHECK(system(cmd) == 0);
}

static void test_under_capacity(void)
{
    printf("  test_under_capacity...\n");
    snprintf(g_root, sizeof(g_root), "/tmp/disco_ok_%d", (int)getpid());
    char plugins[512], cmd[600];
    snprintf(plugins, sizeof(plugins), "%s/plugins", g_root);
    CHECK(mkdir_p(g_root) == 0);
    CHECK(mkdir_p(plugins) == 0);

    for (int i = 0; i < 3; i++)
        CHECK(make_plugin(i) == 0);

    plugin_discovery_config_t cfg = {plugins, false};
    CHECK(plugin_discovery_init(&cfg) == 0);

    plugin_discovery_result_t *results = NULL;
    size_t count = 0;
    CHECK(plugin_discovery_scan(&results, &count) == 0);
    CHECK(count == 3);
    plugin_discovery_free_results(results, count);
    plugin_discovery_destroy();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    CHECK(system(cmd) == 0);
}

#endif /* !_WIN32 */

int main(void)
{
    printf("plugin_discovery capacity tests\n");
#ifndef _WIN32
    test_capacity();
    test_under_capacity();
#else
    printf("  skipped on Windows (POSIX 目录树夹具)\n");
#endif
    if (g_failures) {
        printf("RESULT: FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
