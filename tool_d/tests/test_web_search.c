// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_web_search.c
 * @brief S-P1-06 验收：可配置搜索 provider 的配置读取与 JSON 结果解析。
 *
 * 覆盖：未配置/仅 endpoint/endpoint+key 三种环境状态；Brave、Serper、
 * Bing、根数组等常见响应外壳；非法 JSON、无结果外壳与 max_results 截断。
 * 不做真实网络请求。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin/builtin_net_internal.h"
#include "test_framework.h"

/* 跨平台环境变量写入；value 为 NULL 表示删除。 */
static void set_env(const char *key, const char *value)
{
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value)
        setenv(key, value, 1);
    else
        unsetenv(key);
#endif
}

static void test_provider_cfg(void)
{
    char ep[256], key[64];

    set_env("AIRY_WEB_SEARCH_ENDPOINT", NULL);
    set_env("AIRY_WEB_SEARCH_KEY", NULL);
    assert_int_equal(net_cfg_provider(ep, sizeof(ep), key, sizeof(key)), 0);
    assert_string_equal(ep, "");
    assert_string_equal(key, "");

    set_env("AIRY_WEB_SEARCH_ENDPOINT", "https://api.example.com/search?q=%s");
    assert_int_equal(net_cfg_provider(ep, sizeof(ep), key, sizeof(key)), 1);
    assert_string_equal(ep, "https://api.example.com/search?q=%s");
    assert_string_equal(key, "");

    set_env("AIRY_WEB_SEARCH_KEY", "sk-test-123");
    assert_int_equal(net_cfg_provider(ep, sizeof(ep), key, sizeof(key)), 1);
    assert_string_equal(key, "sk-test-123");

    /* 只问“是否配置”时允许空缓冲区。 */
    assert_int_equal(net_cfg_provider(NULL, 0, NULL, 0), 1);
    set_env("AIRY_WEB_SEARCH_ENDPOINT", NULL);
    set_env("AIRY_WEB_SEARCH_KEY", NULL);
    assert_int_equal(net_cfg_provider(NULL, 0, NULL, 0), 0);
}

static void test_json_brave(void)
{
    const char *json =
        "{\"web\":{\"results\":["
        "{\"title\":\"Alpha\",\"url\":\"https://a.example\",\"description\":\"first\"},"
        "{\"title\":\"Beta\",\"url\":\"https://b.example\",\"description\":\"second\"}]}}";
    char buf[4096] = {0};
    size_t len = 0;

    assert_int_equal(net_search_json(json, 8, buf, sizeof(buf), &len), 2);
    assert_true(len > 0);
    assert_non_null(strstr(buf, "[1] Alpha"));
    assert_non_null(strstr(buf, "https://b.example"));
    assert_non_null(strstr(buf, "second"));
}

static void test_json_serper_and_bing(void)
{
    char buf[2048] = {0};
    size_t len = 0;
    const char *serper =
        "{\"organic_results\":[{\"title\":\"S\",\"link\":\"https://s.example\","
        "\"snippet\":\"snip\"}]}";
    assert_int_equal(net_search_json(serper, 8, buf, sizeof(buf), &len), 1);
    assert_non_null(strstr(buf, "S"));
    assert_non_null(strstr(buf, "https://s.example"));

    memset(buf, 0, sizeof(buf));
    len = 0;
    const char *bing =
        "{\"webPages\":{\"value\":[{\"name\":\"N\",\"url\":\"https://n.example\","
        "\"snippet\":\"bs\"}]}}";
    assert_int_equal(net_search_json(bing, 8, buf, sizeof(buf), &len), 1);
    assert_non_null(strstr(buf, "N"));
}

static void test_json_array_and_limits(void)
{
    char buf[2048] = {0};
    size_t len = 0;
    const char *arr =
        "[{\"title\":\"T1\",\"href\":\"https://t1.example\"},"
        "{\"title\":\"T2\",\"href\":\"https://t2.example\"},"
        "{\"title\":\"T3\",\"href\":\"https://t3.example\"}]";

    assert_int_equal(net_search_json(arr, 2, buf, sizeof(buf), &len), 2);
    assert_non_null(strstr(buf, "T1"));
    assert_non_null(strstr(buf, "T2"));
    assert_null(strstr(buf, "T3"));
}

static void test_json_invalid(void)
{
    char buf[512] = {0};
    size_t len = 0;

    assert_int_equal(net_search_json("not-json", 8, buf, sizeof(buf), &len), 0);
    assert_int_equal(len, 0);
    assert_int_equal(net_search_json("{}", 8, buf, sizeof(buf), &len), 0);
    assert_int_equal(net_search_json("{\"results\":[]}", 8, buf, sizeof(buf), &len), 0);
    assert_int_equal(net_search_json(NULL, 8, buf, sizeof(buf), &len), 0);
}

int main(void)
{
    test_provider_cfg();
    test_json_brave();
    test_json_serper_and_bing();
    test_json_array_and_limits();
    test_json_invalid();
    printf("test_web_search: all checks passed\n");
    return 0;
}
