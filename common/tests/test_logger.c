// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_logger.c
 * @brief 日志模块单元测试（0.1.19 §205 收敛后权威面：logging.h +
 *        svc_logger.h SVC_LOG_* 别名）
 */

#include "svc_logger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    SVC_LOG_INFO("=========================================");
    SVC_LOG_INFO("  Logger Module Unit Tests");
    SVC_LOG_INFO("=========================================");

    test_logger_level_conversion();
    test_logger_init_cleanup();
    test_logger_default_init();

    SVC_LOG_INFO("All logger module tests PASSED");
    return 0;
}
