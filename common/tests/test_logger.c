// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_logger.c
 * @brief 日志模块单元测试（0.1.19 §205 收敛后权威面：logging.h +
 *        svc_logger.h SVC_LOG_* 别名；0.1.20 D2-8 增加 wall/mono 配对断言）
 */

#include "svc_logger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <stdlib.h>
#include <unistd.h>
#endif

static void test_logger_level_conversion(void)
{
    SVC_LOG_INFO("  test_logger_level_conversion...");

    assert(strcmp(log_level_to_string(LOG_LEVEL_DEBUG), "DEBUG") == 0);
    assert(strcmp(log_level_to_string(LOG_LEVEL_INFO), "INFO") == 0);
    assert(strcmp(log_level_to_string(LOG_LEVEL_WARN), "WARN") == 0);
    assert(strcmp(log_level_to_string(LOG_LEVEL_ERROR), "ERROR") == 0);
    assert(strcmp(log_level_to_string(LOG_LEVEL_FATAL), "FATAL") == 0);

    assert(log_level_from_string("DEBUG") == LOG_LEVEL_DEBUG);
    assert(log_level_from_string("INFO") == LOG_LEVEL_INFO);
    assert(log_level_from_string("WARN") == LOG_LEVEL_WARN);
    assert(log_level_from_string("ERROR") == LOG_LEVEL_ERROR);
    assert(log_level_from_string("FATAL") == LOG_LEVEL_FATAL);

    SVC_LOG_INFO("    PASSED");
}

static void test_logger_init_cleanup(void)
{
    SVC_LOG_INFO("  test_logger_init_cleanup...");

    log_config_t config = {0};
    config.level = LOG_LEVEL_DEBUG;
    config.outputs = (1u << LOG_OUTPUT_CONSOLE);

    assert(log_init(&config) == 0);

    SVC_LOG_DEBUG("debug through SVC_LOG_DEBUG");
    SVC_LOG_INFO("info through SVC_LOG_INFO");
    SVC_LOG_WARN("warn through SVC_LOG_WARN");
    SVC_LOG_ERROR("error through SVC_LOG_ERROR");

    log_cleanup();

    SVC_LOG_INFO("    PASSED");
}

static void test_logger_default_init(void)
{
    SVC_LOG_INFO("  test_logger_default_init...");

    assert(log_init(NULL) == 0);
    SVC_LOG_INFO("default config active");
    log_cleanup();

    SVC_LOG_INFO("    PASSED");
}

/* D2-8：每条日志行必须同时携带 wall 与 mono，且同进程内 mono 单调不减。
 * 以 stderr 重定向做端到端断言——只有真实写入路径才覆盖 record 构造。 */
#ifndef _WIN32

static int mono_of_line(const char *line, unsigned long long *out)
{
    const char *p = strstr(line, "] [mono:");
    if (!p)
        return -1;
    char *end = NULL;
    unsigned long long v = strtoull(p + 8, &end, 10);
    if (!end || *end != ']')
        return -1;
    *out = v;
    return 0;
}

static void test_logger_mono_pair(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/airymaxrt-log-mono-%ld.log", (long)getpid());

    fflush(stderr);
    int saved = dup(fileno(stderr));
    assert(saved >= 0);
    assert(freopen(path, "w", stderr) != NULL);

    assert(log_init(NULL) == 0);
    SVC_LOG_INFO("mono pair A");
    SVC_LOG_INFO("mono pair B");
    SVC_LOG_INFO("mono pair C");
    fflush(stderr);

    assert(dup2(saved, fileno(stderr)) >= 0);
    close(saved);
    log_cleanup();

    FILE *f = fopen(path, "r");
    assert(f != NULL);

    int seen = 0;
    unsigned long long prev = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if (mono_of_line(line, &v) != 0)
            continue;
        assert(line[0] == '['); /* wall 段与 mono 段同一行且 wall 在前 */
        if (seen > 0)
            assert(v >= prev);
        prev = v;
        seen++;
    }
    fclose(f);
    remove(path);
    assert(seen == 3);
}

#else

static void test_logger_mono_pair(void)
{
    /* Windows 控制台重定向语义不同，D2-8 由 Linux CI 覆盖 */
}

#endif

int main(void)
{
    SVC_LOG_INFO("=========================================");
    SVC_LOG_INFO("  Logger Module Unit Tests");
    SVC_LOG_INFO("=========================================");

    test_logger_level_conversion();
    test_logger_init_cleanup();
    test_logger_default_init();
    test_logger_mono_pair();

    SVC_LOG_INFO("All logger module tests PASSED");
    return 0;
}
