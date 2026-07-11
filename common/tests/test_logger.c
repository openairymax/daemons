// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_logger.c
 * @brief 日志模块单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "svc_logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_logger_level_conversion(void)
{
    SVC_LOG_INFO("  test_logger_level_conversion...");

    assert(strcmp(airy_log_level_to_string((airy_log_level_t)LOG_LEVEL_DEBUG), "DEBUG") == 0);
    assert(strcmp(airy_log_level_to_string((airy_log_level_t)LOG_LEVEL_INFO), "INFO") == 0);
    assert(strcmp(airy_log_level_to_string((airy_log_level_t)LOG_LEVEL_WARN), "WARN") == 0);
    assert(strcmp(airy_log_level_to_string((airy_log_level_t)LOG_LEVEL_ERROR), "ERROR") == 0);
    assert(strcmp(airy_log_level_to_string((airy_log_level_t)LOG_LEVEL_FATAL), "FATAL") == 0);

    assert(airy_log_level_from_string("DEBUG") == (airy_log_level_t)LOG_LEVEL_DEBUG);
    assert(airy_log_level_from_string("INFO") == (airy_log_level_t)LOG_LEVEL_INFO);
    assert(airy_log_level_from_string("WARN") == (airy_log_level_t)LOG_LEVEL_WARN);
    assert(airy_log_level_from_string("ERROR") == (airy_log_level_t)LOG_LEVEL_ERROR);
    assert(airy_log_level_from_string("FATAL") == (airy_log_level_t)LOG_LEVEL_FATAL);

    SVC_LOG_INFO("    PASSED");
}

static void test_logger_init_shutdown(void)
{
    SVC_LOG_INFO("  test_logger_init_shutdown...");

    airy_logger_config_t config = {.name = "test_agentrt",
                                      .level = (int)LOG_LEVEL_DEBUG,
                                      .targets = NULL,
                                      .target_count = 0,
                                      .include_source = true,
                                      .include_trace = true,
                                      .json_format = false};

    int ret = airy_log_init(&config);
    assert(ret == 0);

    airy_log_set_level((airy_log_level_t)LOG_LEVEL_DEBUG);

    airy_log_shutdown();

    SVC_LOG_INFO("    PASSED");
}

static void test_logger_trace_context(void)
{
    SVC_LOG_INFO("  test_logger_trace_context...");

    airy_trace_context_t ctx;
    airy_trace_new(&ctx);

    assert(ctx.trace_id[0] != '\0');
    assert(strlen(ctx.trace_id) > 0);

    airy_trace_set_current(&ctx);

    const char *current_trace = ctx.trace_id;
    assert(current_trace != NULL);

    airy_trace_set_session_id("test-session-123");
    const char *session_id = airy_trace_get_session_id();
    assert(strcmp(session_id, "test-session-123") == 0);

    SVC_LOG_INFO("    PASSED");
}

static void test_logger_macros(void)
{
    SVC_LOG_INFO("  test_logger_macros...");

    airy_logger_config_t config = {.name = "test_agentrt",
                                      .level = (int)LOG_LEVEL_DEBUG,
                                      .targets = NULL,
                                      .target_count = 0,
                                      .include_source = true,
                                      .include_trace = true,
                                      .json_format = false};

    airy_log_init(&config);

    /* 测试日志宏 */
    LOG_DEBUG("Test debug message: %d", 42);
    LOG_INFO("Test info message");
    LOG_WARN("Test warn message");
    LOG_ERROR("Test error message");

    /* 测试带追踪上下文的日志 */
    airy_trace_context_t ctx;
    airy_trace_new(&ctx);
    LOG_INFO_T(&ctx, "Test message with trace context");

    airy_log_shutdown();

    SVC_LOG_INFO("    PASSED");
}

int main(void)
{
    SVC_LOG_INFO("=========================================");
    SVC_LOG_INFO("  Logger Module Unit Tests");
    SVC_LOG_INFO("=========================================");

    test_logger_level_conversion();
    test_logger_init_shutdown();
    test_logger_trace_context();
    test_logger_macros();

    SVC_LOG_INFO("All logger module tests PASSED");
    return 0;
}
